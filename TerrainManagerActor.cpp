#include "TerrainManagerActor.h"

#include "Async/Async.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformProcess.h"
#include "Materials/MaterialInterface.h"

#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "UDynamicMesh.h"

using UE::Geometry::FDynamicMesh3;

namespace
{
    // Hermite smoothstep. Written locally so this compiles on every 5.x.
    FORCEINLINE float SmoothStep01(float Edge0, float Edge1, float X)
    {
        if (Edge1 <= Edge0) { return X >= Edge1 ? 1.f : 0.f; }
        const float T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.f, 1.f);
        return T * T * (3.f - 2.f * T);
    }

    /**
     * Biomes considered at any one point. Three of them actually contribute; the
     * fourth exists only to supply the floor that gets subtracted off, which is
     * what gives the weight kernel compact support. See SampleTerrain.
     */
    constexpr int32 BiomeCandidates = 4;
    constexpr int32 BiomeContributors = BiomeCandidates - 1;
}

ATerrainManagerActor::ATerrainManagerActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;

    // Single-player: no replication, no authority checks, no OnRep plumbing.
    bReplicates = false;

    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent->SetMobility(EComponentMobility::Static);

    if (Biomes.Num() == 0)
    {
        auto MakeBiome = [](const TCHAR* InName, float T, float M, float Base,
                            float Mtn, float Hill, float Detail,
                            int32 Primary, int32 Secondary, float SecBlend)
        {
            FTerrainBiome B;
            B.Name = InName;
            B.Temperature = T;
            B.Moisture = M;
            B.BaseHeight = Base;
            B.MountainStrength = Mtn;
            B.HillStrength = Hill;
            B.DetailStrength = Detail;
            B.PrimaryLayer = Primary;
            B.SecondaryLayer = Secondary;
            B.SecondaryBlend = SecBlend;
            return B;
        };

        // Layer palette: 0 = grass, 1 = sand/dry dirt, 2 = rock, 3 = snow.
        //                        name          temp  moist  base     mtn   hill  det   pri sec blend
        Biomes.Add(MakeBiome(TEXT("Tundra"),    0.08f, 0.35f,  1500.f, 0.35f, 0.6f, 0.5f,  3,  2, 0.40f));
        Biomes.Add(MakeBiome(TEXT("Alpine"),    0.20f, 0.72f,  9000.f, 1.40f, 1.1f, 0.9f,  2,  0, 0.35f));
        Biomes.Add(MakeBiome(TEXT("Taiga"),     0.36f, 0.80f,  2500.f, 0.80f, 1.2f, 0.9f,  0,  2, 0.30f));
        Biomes.Add(MakeBiome(TEXT("Grassland"), 0.55f, 0.38f,     0.f, 0.10f, 0.4f, 0.4f,  0,  1, 0.12f));
        Biomes.Add(MakeBiome(TEXT("Woodland"),  0.62f, 0.82f,  1200.f, 0.55f, 1.0f, 0.8f,  0,  2, 0.15f));
        Biomes.Add(MakeBiome(TEXT("Savanna"),   0.80f, 0.44f,   500.f, 0.30f, 0.7f, 0.6f,  1,  0, 0.45f));
        Biomes.Add(MakeBiome(TEXT("Badlands"),  0.86f, 0.22f,  4000.f, 1.10f, 0.9f, 1.1f,  2,  1, 0.45f));
        Biomes.Add(MakeBiome(TEXT("Desert"),    0.94f, 0.08f,  -800.f, 0.15f, 0.5f, 1.6f,  1,  2, 0.08f));
    }
}

// ---------------------------------------------------------------- Lifecycle

void ATerrainManagerActor::BeginPlay()
{
    Super::BeginPlay();

    if (Seed == 0)
    {
        Seed = FMath::Rand();
    }

    InitializeNoise();
    UpdateStreaming();
}

void ATerrainManagerActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // Worker jobs read this actor's (immutable) noise objects directly. Blocking
    // here is what makes that safe -- nothing can outlive the actor.
    bShuttingDown.store(true);
    while (ActiveJobs.GetValue() > 0)
    {
        FPlatformProcess::Sleep(0.001f);
    }
    CompletedChunks.Empty();

    Super::EndPlay(EndPlayReason);
}

void ATerrainManagerActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!bNoiseReady)
    {
        return;
    }

    const FIntPoint CurrentChunk = GetFocusChunk();
    if (CurrentChunk != LastFocusChunk)
    {
        LastFocusChunk = CurrentChunk;
        UpdateStreaming();
    }

    DispatchPendingJobs();
    IntegrateCompletedChunks();
}

void ATerrainManagerActor::Regenerate(int32 NewSeed)
{
    bShuttingDown.store(true);
    while (ActiveJobs.GetValue() > 0)
    {
        FPlatformProcess::Sleep(0.001f);
    }
    CompletedChunks.Empty();
    bShuttingDown.store(false);

    for (auto& Pair : Chunks)
    {
        ReleaseMeshComponent(Pair.Value.MeshComponent);
    }
    Chunks.Empty();
    PendingQueue.Reset();
    InFlight.Empty();

    if (NewSeed != 0)
    {
        Seed = NewSeed;
    }

    InitializeNoise();
    LastFocusChunk = FIntPoint(MIN_int32, MIN_int32);
    UpdateStreaming();
}

// -------------------------------------------------------------------- Noise

void ATerrainManagerActor::InitializeNoise()
{
    auto Make = [this](int32 SeedOffset, float Frequency, EFastNoise_NoiseType Type,
                       EFastNoise_FractalType Fractal, int32 Octaves, float Gain)
    {
        UFastNoiseWrapper* N = NewObject<UFastNoiseWrapper>(this);
        N->SetupFastNoise(
            Type,
            Seed + SeedOffset,
            Frequency,
            EFastNoise_Interp::Quintic,
            Fractal,
            Octaves,
            2.0f,   // lacunarity
            Gain,
            0.45f,
            EFastNoise_CellularDistanceFunction::Euclidean,
            EFastNoise_CellularReturnType::CellValue);
        return N;
    };

    using ENT = EFastNoise_NoiseType;
    using EFT = EFastNoise_FractalType;

    NoiseContinent   = Make(   0, ContinentFrequency,     ENT::SimplexFractal, EFT::FBM, 4, 0.50f);
    NoiseRange       = Make( 101, MountainRangeFrequency, ENT::SimplexFractal, EFT::FBM, 3, 0.55f);
    NoiseMountain    = Make( 202, MountainFrequency,      ENT::SimplexFractal, EFT::FBM, 5, 0.50f);
    NoiseHills       = Make( 303, HillFrequency,          ENT::SimplexFractal, EFT::FBM, 4, 0.50f);
    NoiseDetail      = Make( 404, DetailFrequency,        ENT::SimplexFractal, EFT::FBM, 3, 0.45f);
    NoiseWarp        = Make( 505, WarpFrequency,          ENT::SimplexFractal, EFT::FBM, 2, 0.50f);
    NoiseTemperature = Make( 606, TemperatureFrequency,   ENT::SimplexFractal, EFT::FBM, 3, 0.50f);
    NoiseMoisture    = Make( 707, MoistureFrequency,      ENT::SimplexFractal, EFT::FBM, 3, 0.50f);

    bNoiseReady = true;
}

// ------------------------------------------------------------ Terrain field

void ATerrainManagerActor::SampleTerrain(double WorldX, double WorldY,
                                         float& OutHeight, float* OutLayerWeights,
                                         int32* OutDominantBiome) const
{
    if (OutLayerWeights)
    {
        for (int32 i = 0; i < NumSurfaceLayers; ++i) { OutLayerWeights[i] = 0.f; }
        OutLayerWeights[0] = 1.f;
    }
    if (OutDominantBiome)
    {
        *OutDominantBiome = INDEX_NONE;
    }

    if (!bNoiseReady)
    {
        OutHeight = 0.f;
        return;
    }

    // 1. Domain warp. Two extra samples, but it's what turns "noise" into "landscape".
    float SX = (float)WorldX;
    float SY = (float)WorldY;
    if (WarpStrength > KINDA_SMALL_NUMBER)
    {
        const float WX = NoiseWarp->GetNoise2D(SX, SY);
        const float WY = NoiseWarp->GetNoise2D(SX + 5331.7f, SY - 1721.3f);
        SX += WX * WarpStrength;
        SY += WY * WarpStrength;
    }

    // 2. Continental shelf: the very large scale that everything else rides on.
    const float Continent = NoiseContinent->GetNoise2D(SX, SY);
    const float LandMask = SmoothStep01(-0.20f, 0.30f, Continent);

    // 3. Climate. Sampled from the warped domain so biome borders wiggle with the
    //    terrain. The soft saturate is monotonic and asymptotes to +-1, so it
    //    widens the distribution without the clipped plateaus that a clamped
    //    multiply would give you. ClimateContrast = 1 leaves the noise untouched.
    auto SpreadClimate = [this](float N) -> float
    {
        if (ClimateContrast <= 1.f)
        {
            return FMath::Clamp(N * 0.5f + 0.5f, 0.f, 1.f);
        }
        const float S = N * ClimateContrast;
        return FMath::Clamp(0.5f + 0.5f * (S * FMath::InvSqrt(1.f + S * S)), 0.f, 1.f);
    };

    const float Temp  = SpreadClimate(NoiseTemperature->GetNoise2D(SX, SY));
    const float Moist = SpreadClimate(NoiseMoisture->GetNoise2D(SX, SY));

    // 4. Blend biome parameters.
    //
    //    The old code summed 1/(D2*D2 + eps) over *every* biome. That kernel has
    //    infinite support, so all eight biomes contributed everywhere and the
    //    result was a permanent global average -- fine-ish for a height offset,
    //    fatal for anything the material has to key off.
    //
    //    Instead: keep the four strongest, subtract the fourth's weight from the
    //    other three, and normalise. Subtracting the runner-up's floor drives each
    //    survivor smoothly to exactly zero at its own boundary, so the support is
    //    compact and there's still no pop -- the weight that vanishes was already
    //    the smallest one.
    float BaseHeight = 0.f;
    float MountainStrength = 0.5f;
    float HillStrength = 1.f;
    float DetailStrength = 1.f;

    const int32 NumBiomes = Biomes.Num();
    if (NumBiomes > 0)
    {
        float BestW[BiomeCandidates];
        int32 BestI[BiomeCandidates];
        for (int32 s = 0; s < BiomeCandidates; ++s) { BestW[s] = 0.f; BestI[s] = INDEX_NONE; }

        for (int32 i = 0; i < NumBiomes; ++i)
        {
            const FTerrainBiome& B = Biomes[i];
            const float dT = Temp - B.Temperature;
            const float dM = Moist - B.Moisture;

            // Quartic falloff: cheap (no pow/exp) and gives tight cells with soft edges.
            const float D2 = (dT * dT + dM * dM) * BiomeBlendSharpness;
            const float W = 1.f / (D2 * D2 + 1e-3f);

            // Insertion sort into the top-4. Tiny fixed cost, no allocation.
            for (int32 s = 0; s < BiomeCandidates; ++s)
            {
                if (W > BestW[s])
                {
                    for (int32 t = BiomeCandidates - 1; t > s; --t)
                    {
                        BestW[t] = BestW[t - 1];
                        BestI[t] = BestI[t - 1];
                    }
                    BestW[s] = W;
                    BestI[s] = i;
                    break;
                }
            }
        }

        // With fewer than BiomeCandidates biomes the tail stays at 0, so the floor
        // is 0 and every biome present survives. No special case needed.
        const float Floor = BestW[BiomeCandidates - 1];

        // Normalise BEFORE the contrast exponent. The raw weights reach 1000 when
        // D2 -> 0 (the 1e-3 epsilon), and Pow(1000, 8) is 1e24 -- inside float
        // range but it wrecks the precision of the sum. Post-normalisation the
        // inputs are all in 0..1, and since Pow is monotonic the relative contrast
        // after the second normalise is identical.
        float Total = 0.f;
        for (int32 s = 0; s < BiomeContributors; ++s)
        {
            BestW[s] = FMath::Max(0.f, BestW[s] - Floor);
            Total += BestW[s];
        }

        if (Total > SMALL_NUMBER && BiomeContrast > 1.f)
        {
            const float PreInv = 1.f / Total;
            Total = 0.f;
            for (int32 s = 0; s < BiomeContributors; ++s)
            {
                BestW[s] = FMath::Pow(BestW[s] * PreInv, BiomeContrast);
                Total += BestW[s];
            }
        }

        // Degenerate case: the top BiomeCandidates weights are exactly equal, so the
        // floor subtraction zeroed everything. Measure-zero with continuous noise,
        // but falling through to the hardcoded defaults would be a visible pop, so
        // snap to the strongest biome instead.
        if (Total <= SMALL_NUMBER && BestI[0] != INDEX_NONE)
        {
            BestW[0] = 1.f;
            for (int32 s = 1; s < BiomeContributors; ++s) { BestW[s] = 0.f; }
            Total = 1.f;
        }

        if (OutDominantBiome)
        {
            *OutDominantBiome = BestI[0];
        }

        if (Total > SMALL_NUMBER)
        {
            const float Inv = 1.f / Total;

            float AccBase = 0.f, AccMtn = 0.f, AccHill = 0.f, AccDetail = 0.f;

            // Not "Layers" -- AActor already has a TArray<FName> Layers member for
            // editor layer visibility, and C4458 is an error in this project.
            float AccLayers[NumSurfaceLayers] = { 0.f, 0.f, 0.f, 0.f };

            for (int32 s = 0; s < BiomeContributors; ++s)
            {
                if (BestI[s] == INDEX_NONE || BestW[s] <= 0.f)
                {
                    continue;
                }

                const FTerrainBiome& B = Biomes[BestI[s]];
                const float W = BestW[s] * Inv;

                AccBase   += B.BaseHeight       * W;
                AccMtn    += B.MountainStrength * W;
                AccHill   += B.HillStrength     * W;
                AccDetail += B.DetailStrength   * W;

                const float Sec = FMath::Clamp(B.SecondaryBlend, 0.f, 1.f);
                AccLayers[FMath::Clamp(B.PrimaryLayer,   0, NumSurfaceLayers - 1)] += W * (1.f - Sec);
                AccLayers[FMath::Clamp(B.SecondaryLayer, 0, NumSurfaceLayers - 1)] += W * Sec;
            }

            BaseHeight       = AccBase;
            MountainStrength = AccMtn;
            HillStrength     = AccHill;
            DetailStrength   = AccDetail;

            if (OutLayerWeights)
            {
                // Already a partition of unity by construction, but renormalise so
                // float drift can never send a not-quite-1 sum to the material.
                float Sum = 0.f;
                for (int32 i = 0; i < NumSurfaceLayers; ++i) { Sum += AccLayers[i]; }
                const float InvSum = (Sum > SMALL_NUMBER) ? (1.f / Sum) : 0.f;
                for (int32 i = 0; i < NumSurfaceLayers; ++i)
                {
                    OutLayerWeights[i] = AccLayers[i] * InvSum;
                }
                if (InvSum == 0.f) { OutLayerWeights[0] = 1.f; }
            }
        }
    }

    // 5. Mountain ranges. A low-frequency mask localises the ridges so you get
    //    real ranges with plains between them instead of uniform crumple.
    const float RangeRaw = NoiseRange->GetNoise2D(SX, SY) * 0.5f + 0.5f;
    const float RangeMask = SmoothStep01(MountainRangeThreshold, 1.f, RangeRaw);

    float Ridge = 1.f - FMath::Abs(NoiseMountain->GetNoise2D(SX, SY));
    Ridge *= Ridge;   // sharpen: 0..1 with knife-edge crests

    const float Hills = NoiseHills->GetNoise2D(SX, SY);

    // NOTE: no LOD-based attenuation here. Any term that varies with LOD makes two
    // neighbouring chunks disagree about the height at the *same* world position,
    // which is a guaranteed visible crack. Height must depend on XY alone.
    const float Detail = NoiseDetail->GetNoise2D(SX, SY);

    float Height =
          Continent * ContinentAmplitude
        + Ridge * RangeMask * LandMask * MountainStrength * MountainAmplitude
        + Hills * HillStrength * HillAmplitude
        + Detail * DetailStrength * DetailAmplitude
        + BaseHeight * LandMask;

    if (HeightRemapCurve && HeightRemapRange > KINDA_SMALL_NUMBER)
    {
        const float Norm = FMath::Clamp(Height / HeightRemapRange, -1.f, 1.f);
        Height = HeightRemapCurve->GetFloatValue(Norm) * HeightRemapRange;
    }

    OutHeight = Height;
}

float ATerrainManagerActor::GetHeight(float WorldX, float WorldY) const
{
    float H = 0.f;
    SampleTerrain(WorldX, WorldY, H, nullptr);
    return H;
}

FLinearColor ATerrainManagerActor::GetSurfaceLayerWeights(float WorldX, float WorldY) const
{
    float H = 0.f;
    float L[NumSurfaceLayers] = { 0.f, 0.f, 0.f, 0.f };
    SampleTerrain(WorldX, WorldY, H, L);
    return FLinearColor(L[0], L[1], L[2], L[3]);
}

FName ATerrainManagerActor::GetBiomeAt(float WorldX, float WorldY) const
{
    float H = 0.f;
    int32 Dominant = INDEX_NONE;
    SampleTerrain(WorldX, WorldY, H, nullptr, &Dominant);
    return Biomes.IsValidIndex(Dominant) ? Biomes[Dominant].Name : NAME_None;
}

void ATerrainManagerActor::LogBiomeCoverage(int32 GridSamples, float WorldExtent) const
{
    const int32 N = FMath::Clamp(GridSamples, 4, 1024);
    const int32 NumBiomes = Biomes.Num();
    if (NumBiomes == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[Terrain] No biomes configured."));
        return;
    }

    TArray<int32> Counts;
    Counts.SetNumZeroed(NumBiomes);
    int32 Unassigned = 0;

    const FVector Origin = GetActorLocation();
    const double Half = (double)WorldExtent * 0.5;
    const double StepW = (double)WorldExtent / (double)(N - 1);

    for (int32 ix = 0; ix < N; ++ix)
    {
        for (int32 iy = 0; iy < N; ++iy)
        {
            float H = 0.f;
            int32 Dominant = INDEX_NONE;
            SampleTerrain(Origin.X - Half + ix * StepW,
                          Origin.Y - Half + iy * StepW, H, nullptr, &Dominant);

            if (Counts.IsValidIndex(Dominant)) { ++Counts[Dominant]; }
            else { ++Unassigned; }
        }
    }

    const float TotalSamples = (float)(N * N);
    UE_LOG(LogTemp, Log, TEXT("[Terrain] Biome coverage over %.1f km, %dx%d samples:"),
           WorldExtent / 100000.f, N, N);

    for (int32 i = 0; i < NumBiomes; ++i)
    {
        const float Pct = 100.f * (float)Counts[i] / TotalSamples;
        UE_LOG(LogTemp, Log, TEXT("  %-12s T=%.2f M=%.2f  %6.2f%%%s"),
               *Biomes[i].Name.ToString(), Biomes[i].Temperature, Biomes[i].Moisture, Pct,
               Counts[i] == 0 ? TEXT("   <-- UNREACHABLE") : TEXT(""));
    }

    if (Unassigned > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("  %d samples had no dominant biome."), Unassigned);
    }
}

// ---------------------------------------------------------------- Streaming

FVector ATerrainManagerActor::GetFocusLocation() const
{
    if (UWorld* World = GetWorld())
    {
        if (APlayerController* PC = World->GetFirstPlayerController())
        {
            if (PC->PlayerCameraManager)
            {
                return PC->PlayerCameraManager->GetCameraLocation();
            }
            if (APawn* Pawn = PC->GetPawn())
            {
                return Pawn->GetActorLocation();
            }
        }
    }
    return GetActorLocation();
}

FIntPoint ATerrainManagerActor::GetFocusChunk() const
{
    const FVector Loc = GetFocusLocation();
    return FIntPoint(
        FMath::FloorToInt32(Loc.X / (double)ChunkSize),
        FMath::FloorToInt32(Loc.Y / (double)ChunkSize));
}

int32 ATerrainManagerActor::ComputeLOD(float ChunkDistance) const
{
    int32 LOD = 0;
    for (int32 i = 0; i < LODRingDistances.Num(); ++i)
    {
        if (ChunkDistance > LODRingDistances[i])
        {
            LOD = i + 1;
        }
    }
    return FMath::Clamp(LOD, 0, MaxLOD);
}

FIntVector4 ATerrainManagerActor::ComputeNeighborLODs(FIntPoint Coord, FIntPoint Center) const
{
    auto LODAt = [this, Center](int32 X, int32 Y)
    {
        const float dx = (float)(X - Center.X);
        const float dy = (float)(Y - Center.Y);
        return ComputeLOD(FMath::Sqrt(dx * dx + dy * dy));
    };

    return FIntVector4(
        LODAt(Coord.X - 1, Coord.Y),
        LODAt(Coord.X + 1, Coord.Y),
        LODAt(Coord.X, Coord.Y - 1),
        LODAt(Coord.X, Coord.Y + 1));
}

void ATerrainManagerActor::UpdateStreaming()
{
    const FIntPoint Center = GetFocusChunk();
    const float RadiusSq = (float)(ChunkLoadRadius * ChunkLoadRadius);

    TSet<FIntPoint> Desired;
    Desired.Reserve((2 * ChunkLoadRadius + 1) * (2 * ChunkLoadRadius + 1));

    // Rebuild the queue from scratch: stale requests from the previous position
    // are dropped rather than generated and immediately thrown away.
    PendingQueue.Reset();

    for (int32 dx = -ChunkLoadRadius; dx <= ChunkLoadRadius; ++dx)
    {
        for (int32 dy = -ChunkLoadRadius; dy <= ChunkLoadRadius; ++dy)
        {
            const float DistSq = (float)(dx * dx + dy * dy);
            if (DistSq > RadiusSq)
            {
                continue;   // circular footprint, ~21% fewer chunks than a square
            }

            const FIntPoint Coord(Center.X + dx, Center.Y + dy);
            Desired.Add(Coord);

            const float Dist = FMath::Sqrt(DistSq);
            const int32 WantLOD = ComputeLOD(Dist);
            const FIntVector4 WantNeighbors = ComputeNeighborLODs(Coord, Center);

            const FTerrainChunk* Existing = Chunks.Find(Coord);
            if (Existing && Existing->LOD == WantLOD && Existing->NeighborLODs == WantNeighbors)
            {
                continue;
            }
            if (InFlight.Contains(Coord))
            {
                continue;   // re-checked when it lands
            }

            PendingQueue.Add({ Coord, WantLOD, WantNeighbors, Dist });
        }
    }

    // Furthest first, so DispatchPendingJobs can Pop() the nearest off the end.
    PendingQueue.Sort([](const FPendingChunk& A, const FPendingChunk& B)
    {
        return A.Distance > B.Distance;
    });

    // Recycle everything outside the radius.
    TArray<FIntPoint> ToRemove;
    for (auto& Pair : Chunks)
    {
        if (!Desired.Contains(Pair.Key))
        {
            ReleaseMeshComponent(Pair.Value.MeshComponent);
            ToRemove.Add(Pair.Key);
        }
    }
    for (const FIntPoint& Key : ToRemove)
    {
        Chunks.Remove(Key);
    }
}

void ATerrainManagerActor::DispatchPendingJobs()
{
    while (PendingQueue.Num() > 0 && ActiveJobs.GetValue() < MaxConcurrentJobs)
    {
        const FPendingChunk Job = PendingQueue.Pop(EAllowShrinking::No);

        if (InFlight.Contains(Job.Coord))
        {
            continue;
        }
        InFlight.Add(Job.Coord);
        ActiveJobs.Increment();

        const FIntPoint Coord = Job.Coord;
        const int32 LOD = Job.LOD;
        const FIntVector4 NeighborLODs = Job.NeighborLODs;

        Async(EAsyncExecution::ThreadPool, [this, Coord, LOD, NeighborLODs]()
        {
            if (!bShuttingDown.load())
            {
                TUniquePtr<FTerrainChunkResult> Result = MakeUnique<FTerrainChunkResult>();
                Result->Coord = Coord;
                Result->LOD = LOD;
                Result->NeighborLODs = NeighborLODs;
                BuildChunkMesh(Coord, LOD, NeighborLODs, Result->Mesh);
                CompletedChunks.Enqueue(MoveTemp(Result));
            }
            ActiveJobs.Decrement();
        });
    }
}

void ATerrainManagerActor::IntegrateCompletedChunks()
{
    int32 Budget = FMath::Max(1, MaxChunkUploadsPerFrame);

    TUniquePtr<FTerrainChunkResult> Item;
    while (Budget > 0 && CompletedChunks.Dequeue(Item))
    {
        InFlight.Remove(Item->Coord);

        // The player may have walked off while this was cooking.
        const FIntPoint Center = GetFocusChunk();
        const int32 dx = Item->Coord.X - Center.X;
        const int32 dy = Item->Coord.Y - Center.Y;
        const float Dist = FMath::Sqrt((float)(dx * dx + dy * dy));

        if (Dist > (float)ChunkLoadRadius)
        {
            continue;   // discarded, component never touched
        }

        const int32 WantLOD = ComputeLOD(Dist);
        const FIntVector4 WantNeighbors = ComputeNeighborLODs(Item->Coord, Center);
        if (WantLOD != Item->LOD || WantNeighbors != Item->NeighborLODs)
        {
            // Stale detail level or stale stitching -- requeue rather than uploading
            // something we would immediately replace.
            PendingQueue.Add({ Item->Coord, WantLOD, WantNeighbors, Dist });
            continue;
        }

        ApplyChunk(*Item);
        --Budget;
    }
}

// ------------------------------------------------------------- Mesh builder

void ATerrainManagerActor::BuildChunkMesh(FIntPoint Coord, int32 LOD, const FIntVector4& NeighborLODs,
                                          FDynamicMesh3& OutMesh) const
{
    const int32 Res = FMath::Max(2, VerticesPerChunk >> FMath::Clamp(LOD, 0, 6));
    const double Step = (double)ChunkSize / (double)Res;
    const double OriginX = (double)Coord.X * (double)ChunkSize;
    const double OriginY = (double)Coord.Y * (double)ChunkSize;

    // Normals are always differenced over the LOD-0 spacing, never the chunk's own
    // spacing. If the epsilon varied with LOD, two chunks meeting at a ring boundary
    // would light the same vertex differently -- a seam with no geometric gap at all.
    const double NormalEps = (double)ChunkSize / (double)FMath::Max(4, VerticesPerChunk);
    const float InvTwoEps = (float)(1.0 / (2.0 * NormalEps));

    // Sample with a one-cell border on every side. At LOD 0 that border gives normals
    // for free, since Step already equals NormalEps.
    const int32 Stride = Res + 3;
    auto HIdx = [Stride](int32 ix, int32 iy) { return (ix + 1) * Stride + (iy + 1); };

    TArray<float> Heights;
    TArray<FVector4f> LayerWeights;
    Heights.SetNumUninitialized(Stride * Stride);
    LayerWeights.SetNumUninitialized(Stride * Stride);

    for (int32 ix = -1; ix <= Res + 1; ++ix)
    {
        for (int32 iy = -1; iy <= Res + 1; ++iy)
        {
            const int32 Idx = HIdx(ix, iy);
            float L[NumSurfaceLayers] = { 0.f, 0.f, 0.f, 0.f };
            SampleTerrain(OriginX + ix * Step, OriginY + iy * Step, Heights[Idx], L);
            LayerWeights[Idx] = FVector4f(L[0], L[1], L[2], L[3]);
        }
    }

    OutMesh.Clear();
    OutMesh.EnableAttributes();

    const int32 Side = Res + 1;
    const int32 GridVerts = Side * Side;

    TArray<FVector3f> VNormals;
    TArray<FVector2f> VUVs;
    TArray<FVector4f> VColors;
    VNormals.Reserve(GridVerts * 2);
    VUVs.Reserve(GridVerts * 2);
    VColors.Reserve(GridVerts * 2);

    // Wrap the UV origin into a small range so far-out chunks don't lose float
    // precision. 1024 whole tiles is an invisible step in the tiling pattern.
    const double UOrigin = FMath::Fmod(OriginX / (double)UVTileSize, 1024.0);
    const double VOrigin = FMath::Fmod(OriginY / (double)UVTileSize, 1024.0);

    const float InvTwoStep = (float)(1.0 / (2.0 * Step));
    const bool bNormalsFromGrid = (LOD == 0);

    for (int32 ix = 0; ix <= Res; ++ix)
    {
        for (int32 iy = 0; iy <= Res; ++iy)
        {
            const int32 Idx = HIdx(ix, iy);
            const float H = Heights[Idx];
            const double WX = OriginX + ix * Step;
            const double WY = OriginY + iy * Step;

            // Local space. Keeps vertex coords small even a million units from origin.
            OutMesh.AppendVertex(FVector3d(ix * Step, iy * Step, H));

            FVector3f N;
            if (bNormalsFromGrid)
            {
                const float HL = Heights[HIdx(ix - 1, iy)];
                const float HR = Heights[HIdx(ix + 1, iy)];
                const float HD = Heights[HIdx(ix, iy - 1)];
                const float HU = Heights[HIdx(ix, iy + 1)];
                N = FVector3f((HL - HR) * InvTwoStep, (HD - HU) * InvTwoStep, 1.f);
            }
            else
            {
                float HL, HR, HD, HU;
                SampleTerrain(WX - NormalEps, WY, HL, nullptr);
                SampleTerrain(WX + NormalEps, WY, HR, nullptr);
                SampleTerrain(WX, WY - NormalEps, HD, nullptr);
                SampleTerrain(WX, WY + NormalEps, HU, nullptr);
                N = FVector3f((HL - HR) * InvTwoEps, (HD - HU) * InvTwoEps, 1.f);
            }
            N.Normalize();
            VNormals.Add(N);

            VUVs.Add(FVector2f(
                (float)(UOrigin + (ix * Step) / (double)UVTileSize),
                (float)(VOrigin + (iy * Step) / (double)UVTileSize)));

            // Vertex colour is the layer weight set, nothing else. Rock-on-slope and
            // snow-at-altitude used to be lerped in here; they now live in the
            // material, where they get per-pixel resolution instead of one sample
            // every Step cm, and where they no longer contaminate the biome signal.
            VColors.Add(LayerWeights[Idx]);
        }
    }

    // ---- Edge stitching.
    // Where a neighbour is coarser, collapse our extra border vertices onto the
    // straight segment the neighbour actually renders. Because SampleTerrain is
    // LOD-free, our i0/i1 heights are identical to the neighbour's, so the two
    // borders become the same polyline -- the crack is gone, not just covered.
    {
        auto SnapVertex = [&](int32 VID, int32 VID0, int32 VID1, float T)
        {
            FVector3d P = OutMesh.GetVertex(VID);
            const double Z0 = OutMesh.GetVertex(VID0).Z;
            const double Z1 = OutMesh.GetVertex(VID1).Z;
            P.Z = FMath::Lerp(Z0, Z1, (double)T);
            OutMesh.SetVertex(VID, P);

            // Shading is interpolated the same way, so lighting matches the
            // neighbour across the join too.
            const FVector3f N0 = VNormals[VID0];
            const FVector3f N1 = VNormals[VID1];
            FVector3f N = FMath::Lerp(N0, N1, T);
            N.Normalize();
            VNormals[VID] = N;

            // Layer weights lerp correctly: a convex combination of two partitions
            // of unity is still a partition of unity, which is exactly the property
            // the height-blend node in the material relies on.
            const FVector4f C0 = VColors[VID0];
            const FVector4f C1 = VColors[VID1];
            VColors[VID] = FMath::Lerp(C0, C1, T);
        };

        auto StitchEdge = [&](int32 NeighborLOD, bool bVaryX, int32 FixedIndex)
        {
            const int32 Delta = NeighborLOD - LOD;
            if (Delta <= 0)
            {
                return;   // neighbour is equal or finer; it stitches to us instead
            }

            const int32 K = 1 << FMath::Clamp(Delta, 1, 6);
            if (K >= Res)
            {
                return;   // degenerate; the skirt covers this
            }

            auto VIDAt = [&](int32 i)
            {
                return bVaryX ? (i * Side + FixedIndex) : (FixedIndex * Side + i);
            };

            for (int32 i = 1; i < Res; ++i)
            {
                const int32 m = i % K;
                if (m == 0)
                {
                    continue;   // shared with the neighbour already
                }
                SnapVertex(VIDAt(i), VIDAt(i - m), VIDAt(i - m + K), (float)m / (float)K);
            }
        };

        // Corners sit at index 0 and Res, which are multiples of every K, so they are
        // never snapped and the two edges meeting there can't disagree.
        StitchEdge(NeighborLODs.X, false, 0);     // -X
        StitchEdge(NeighborLODs.Y, false, Res);   // +X
        StitchEdge(NeighborLODs.Z, true,  0);     // -Y
        StitchEdge(NeighborLODs.W, true,  Res);   // +Y
    }

    for (int32 ix = 0; ix < Res; ++ix)
    {
        for (int32 iy = 0; iy < Res; ++iy)
        {
            const int32 I0 = ix * Side + iy;
            const int32 I1 = I0 + 1;
            const int32 I2 = I0 + Side;
            const int32 I3 = I2 + 1;

            OutMesh.AppendTriangle(I0, I1, I2);
            OutMesh.AppendTriangle(I1, I3, I2);
        }
    }

    // ---- Skirts: a shallow lip around the border. With stitching in place this is
    // only a safety net against float differences, so it stays small at every LOD
    // instead of scaling up and exposing a huge smeared wall on distant chunks.
    if (SkirtDepth > KINDA_SMALL_NUMBER)
    {
        auto AddSkirt = [&](const TArray<int32>& Edge, bool bFlip)
        {
            TArray<int32> Lower;
            Lower.Reserve(Edge.Num());

            for (int32 VID : Edge)
            {
                FVector3d P = OutMesh.GetVertex(VID);
                P.Z -= SkirtDepth;
                Lower.Add(OutMesh.AppendVertex(P));

                // Copy to locals first: TArray::Add asserts if the source element
                // lives inside the array being appended to.
                const FVector3f N = VNormals[VID];
                const FVector2f UV = VUVs[VID];
                const FVector4f C = VColors[VID];

                VNormals.Add(N);
                VUVs.Add(UV);
                VColors.Add(C);
            }

            for (int32 i = 0; i + 1 < Edge.Num(); ++i)
            {
                if (bFlip)
                {
                    OutMesh.AppendTriangle(Edge[i], Edge[i + 1], Lower[i]);
                    OutMesh.AppendTriangle(Edge[i + 1], Lower[i + 1], Lower[i]);
                }
                else
                {
                    OutMesh.AppendTriangle(Edge[i], Lower[i], Edge[i + 1]);
                    OutMesh.AppendTriangle(Edge[i + 1], Lower[i], Lower[i + 1]);
                }
            }
        };

        TArray<int32> E;
        E.Reserve(Side);

        E.Reset(); for (int32 ix = 0; ix <= Res; ++ix) { E.Add(ix * Side + 0);   }  AddSkirt(E, true);   // Y-
        E.Reset(); for (int32 ix = 0; ix <= Res; ++ix) { E.Add(ix * Side + Res); }  AddSkirt(E, false);  // Y+
        E.Reset(); for (int32 iy = 0; iy <= Res; ++iy) { E.Add(0 * Side + iy);   }  AddSkirt(E, false);  // X-
        E.Reset(); for (int32 iy = 0; iy <= Res; ++iy) { E.Add(Res * Side + iy); }  AddSkirt(E, true);   // X+
    }

    // ---- Attribute overlays.
    // The mesh is compact and vertices were appended in order, so element i maps to vertex i.
    UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = OutMesh.Attributes()->PrimaryNormals();
    UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = OutMesh.Attributes()->PrimaryUV();
    OutMesh.Attributes()->EnablePrimaryColors();
    UE::Geometry::FDynamicMeshColorOverlay* ColorOverlay = OutMesh.Attributes()->PrimaryColors();

    const int32 NumVerts = VNormals.Num();
    for (int32 i = 0; i < NumVerts; ++i)
    {
        NormalOverlay->AppendElement(VNormals[i]);
        UVOverlay->AppendElement(VUVs[i]);
        ColorOverlay->AppendElement(VColors[i]);
    }

    for (int32 TID : OutMesh.TriangleIndicesItr())
    {
        const UE::Geometry::FIndex3i Tri = OutMesh.GetTriangle(TID);
        NormalOverlay->SetTriangle(TID, Tri);
        UVOverlay->SetTriangle(TID, Tri);
        ColorOverlay->SetTriangle(TID, Tri);
    }
}

// ------------------------------------------------------- Component handling

UDynamicMeshComponent* ATerrainManagerActor::AcquireMeshComponent()
{
    UDynamicMeshComponent* Comp = nullptr;

    if (ComponentPool.Num() > 0)
    {
        Comp = ComponentPool.Pop(EAllowShrinking::No);
        Comp->SetVisibility(true);
    }
    else
    {
        Comp = NewObject<UDynamicMeshComponent>(this);
        Comp->SetMobility(EComponentMobility::Movable);
        Comp->SetupAttachment(RootComponent);
        Comp->RegisterComponent();

        Comp->SetTangentsType(EDynamicMeshComponentTangentsMode::AutoCalculated);
        Comp->SetEnableFlatShading(false);

        // CRITICAL for layer weights. UDynamicMeshComponent can apply a colour-space
        // transform when it converts overlay colours into the render buffer. That is
        // right for colours and wrong for weights -- an sRGB->linear pass turns 0.5
        // into 0.21, a linear->sRGB pass turns it into 0.73, and either way the
        // material gets numbers that no longer sum to 1.
        //
        // If this line doesn't compile on your engine version, look for
        // ColorSpaceMode / SetVertexColorSpaceTransformMode on the component and pick
        // whichever enumerator means "leave it alone".
        Comp->SetVertexColorSpaceTransformMode(EDynamicMeshVertexColorTransformMode::NoTransform);

        // Collision is cooked off-thread and only when we explicitly ask for it.
        Comp->bUseAsyncCooking = true;
        Comp->bDeferCollisionUpdates = true;
        Comp->bEnableComplexCollision = true;
        Comp->CollisionType = ECollisionTraceFlag::CTF_UseComplexAsSimple;
        Comp->SetCollisionProfileName(TEXT("BlockAll"));
    }

    // Set on every acquire, not just on creation. Pooled components outlive a
    // Regenerate, so if TerrainMaterial changed in between, a recycled component
    // would otherwise keep rendering with the old one -- which looks exactly like
    // "my material edit didn't take" while you're iterating.
    Comp->SetMaterial(0, TerrainMaterial);
    Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    return Comp;
}

void ATerrainManagerActor::ReleaseMeshComponent(UDynamicMeshComponent* Comp)
{
    if (!Comp)
    {
        return;
    }

    Comp->SetVisibility(false);
    Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // Recycling beats destroy/create: no render proxy churn, no reallocation.
    // Moving one chunk retires roughly one ring's worth of components, and a
    // Regenerate retires the whole disc at once, so the old max(16, radius*4)
    // cap meant almost everything got destroyed and rebuilt.
    const int32 PoolCap = FMath::Max(64, ChunkLoadRadius * ChunkLoadRadius * 2);
    if (ComponentPool.Num() < PoolCap)
    {
        ComponentPool.Add(Comp);
    }
    else
    {
        Comp->DestroyComponent();
    }
}

void ATerrainManagerActor::ApplyChunk(FTerrainChunkResult& Result)
{
    FTerrainChunk* Existing = Chunks.Find(Result.Coord);
    UDynamicMeshComponent* Comp = Existing ? Existing->MeshComponent.Get() : nullptr;
    if (!Comp)
    {
        // Covers both the new-chunk case and a tracked chunk whose component went
        // away underneath us. Bailing out on the latter would strand that coord
        // permanently: it stays in Chunks, so UpdateStreaming keeps seeing a LOD
        // mismatch and requeueing it, and it never renders.
        Comp = AcquireMeshComponent();
        if (!Comp)
        {
            return;
        }
        if (Existing)
        {
            Existing->MeshComponent = Comp;
        }
    }

    Comp->SetWorldLocation(FVector(
        (double)Result.Coord.X * ChunkSize,
        (double)Result.Coord.Y * ChunkSize,
        0.0));

    Comp->SetCastShadow(Result.LOD <= ShadowMaxLOD);

    Comp->GetDynamicMesh()->SetMesh(MoveTemp(Result.Mesh));

    const bool bWantCollision = (Result.LOD <= CollisionMaxLOD);
    if (bWantCollision)
    {
        Comp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Comp->UpdateCollision(false);
    }
    else
    {
        Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }

    if (Existing)
    {
        Existing->LOD = Result.LOD;
        Existing->NeighborLODs = Result.NeighborLODs;
    }
    else
    {
        FTerrainChunk NewChunk;
        NewChunk.MeshComponent = Comp;
        NewChunk.LOD = Result.LOD;
        NewChunk.NeighborLODs = Result.NeighborLODs;
        Chunks.Add(Result.Coord, NewChunk);
    }
}
