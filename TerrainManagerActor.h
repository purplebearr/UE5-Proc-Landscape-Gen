#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "FastNoiseWrapper.h"
#include "Containers/Queue.h"
#include "HAL/ThreadSafeCounter.h"
#include "Curves/CurveFloat.h"

#include <atomic>

#include "TerrainManagerActor.generated.h"

/**
 * ---------------------------------------------------------------------------
 * SURFACE LAYER PALETTE
 * ---------------------------------------------------------------------------
 * Vertex colour no longer carries a blended biome *tint*. It carries four
 * normalised *layer weights* -- RGBA maps to layers 0..3 -- which the material
 * uses to height-blend four texture sets.
 *
 * The palette is a material-side convention. The default biome table assumes:
 *
 *      0 (R) = grass / verdant ground cover
 *      1 (G) = sand / dry dirt
 *      2 (B) = rock / gravel
 *      3 (A) = snow
 *
 * Rock-on-slope and snow-at-altitude are NOT baked here any more. They are
 * altitude and slope effects, not climate effects, and doing them per-pixel in
 * the material (VertexNormalWS.B for slope, AbsoluteWorldPosition.B for height)
 * is both crisper and keeps the biome signal clean.
 */

/**
 * A biome is defined by a point in climate space (temperature / moisture).
 * Terrain parameters are blended between biomes by climate distance, so
 * there are never hard seams -- only smooth transitions.
 */
USTRUCT(BlueprintType)
struct FTerrainBiome
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
    FName Name = NAME_None;

    /** Climate cell centre, 0..1. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Temperature = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float Moisture = 0.5f;

    /** Flat height offset applied inside this biome, in cm. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
    float BaseHeight = 0.f;

    /** How much of the global ridged-mountain field shows through here. 0 = dead flat, 1 = full alpine. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.0"))
    float MountainStrength = 0.5f;

    /** Mid-scale rolling hills multiplier. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.0"))
    float HillStrength = 1.f;

    /** High-frequency surface roughness multiplier (dunes, rubble, bumps). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.0"))
    float DetailStrength = 1.f;

    /** Dominant surface layer. Index into the material's 4-layer palette. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Surface", meta = (ClampMin = "0", ClampMax = "3"))
    int32 PrimaryLayer = 0;

    /** Secondary layer mixed in, for biomes that aren't a single clean material. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Surface", meta = (ClampMin = "0", ClampMax = "3"))
    int32 SecondaryLayer = 0;

    /** 0 = pure primary, 1 = pure secondary. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome|Surface", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float SecondaryBlend = 0.f;
};

USTRUCT()
struct FTerrainChunk
{
    GENERATED_BODY()

    UPROPERTY()
    TObjectPtr<UDynamicMeshComponent> MeshComponent = nullptr;

    int32 LOD = 0;

    /** LOD of the -X, +X, -Y, +Y neighbours at build time. Drives edge stitching. */
    FIntVector4 NeighborLODs = FIntVector4(0, 0, 0, 0);
};

/** Result handed back from a worker thread. Not a USTRUCT -- lives only in queues. */
struct FTerrainChunkResult
{
    FIntPoint Coord = FIntPoint::ZeroValue;
    int32 LOD = 0;
    FIntVector4 NeighborLODs = FIntVector4(0, 0, 0, 0);
    UE::Geometry::FDynamicMesh3 Mesh;
};

UCLASS()
class PROJECTNAME_API ATerrainManagerActor : public AActor
{
    GENERATED_BODY()

public:

    ATerrainManagerActor();

    /** Number of surface layers packed into vertex colour RGBA. */
    static constexpr int32 NumSurfaceLayers = 4;

protected:

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaTime) override;

public:

    // ---------------------------------------------------------------- World

    /** Set to 0 to randomise on BeginPlay. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
    int32 Seed = 1337;

    /** Chunk footprint in cm. 25600 = 256 m. */
    UPROPERTY(EditAnywhere, Category = "Terrain")
    int32 ChunkSize = 25600;

    /** Quads per chunk edge at LOD 0. Use a power of two. */
    UPROPERTY(EditAnywhere, Category = "Terrain", meta = (ClampMin = "4"))
    int32 VerticesPerChunk = 64;

    UPROPERTY(EditAnywhere, Category = "Terrain")
    TObjectPtr<UMaterialInterface> TerrainMaterial = nullptr;

    /** World cm covered by one tile of the material's UVs. */
    UPROPERTY(EditAnywhere, Category = "Terrain")
    float UVTileSize = 2048.f;

    /** Optional artistic remap. Input and output are both normalised -1..1 around sea level. */
    UPROPERTY(EditAnywhere, Category = "Terrain")
    TObjectPtr<UCurveFloat> HeightRemapCurve = nullptr;

    /** Height that HeightRemapCurve normalises against. */
    UPROPERTY(EditAnywhere, Category = "Terrain")
    float HeightRemapRange = 80000.f;

    // ------------------------------------------------------------ Streaming

    /** Radius in chunks. 10 @ 25600 cm ~= 2.5 km of visible terrain. */
    UPROPERTY(EditAnywhere, Category = "Terrain|Streaming", meta = (ClampMin = "1"))
    int32 ChunkLoadRadius = 10;

    /**
     * Chunk-distance at which each LOD ends. {1,2,4,7} means:
     * LOD0 <= 1, LOD1 <= 2, LOD2 <= 4, LOD3 <= 7, LOD4 beyond.
     */
    UPROPERTY(EditAnywhere, Category = "Terrain|Streaming")
    TArray<float> LODRingDistances = { 1.f, 2.f, 4.f, 7.f };

    UPROPERTY(EditAnywhere, Category = "Terrain|Streaming", meta = (ClampMin = "0", ClampMax = "6"))
    int32 MaxLOD = 4;

    /** Chunks at this LOD or closer get collision cooked. Everything else is visual only. */
    UPROPERTY(EditAnywhere, Category = "Terrain|Streaming", meta = (ClampMin = "0"))
    int32 CollisionMaxLOD = 1;

    /** Chunks past this LOD stop casting dynamic shadows. */
    UPROPERTY(EditAnywhere, Category = "Terrain|Streaming", meta = (ClampMin = "0"))
    int32 ShadowMaxLOD = 2;

    /** Worker-thread mesh builds allowed at once. */
    UPROPERTY(EditAnywhere, Category = "Terrain|Streaming", meta = (ClampMin = "1"))
    int32 MaxConcurrentJobs = 4;

    /** Meshes pushed to the render thread per frame. Keep low -- this is what causes hitches. */
    UPROPERTY(EditAnywhere, Category = "Terrain|Streaming", meta = (ClampMin = "1"))
    int32 MaxChunkUploadsPerFrame = 2;

    /**
     * Small safety lip around each chunk border, in cm. Edge stitching already
     * removes LOD cracks, so this only needs to cover float noise. 0 disables it.
     */
    UPROPERTY(EditAnywhere, Category = "Terrain|Streaming", meta = (ClampMin = "0.0"))
    float SkirtDepth = 500.f;

    // ---------------------------------------------------------------- Noise

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float ContinentFrequency = 0.0000015f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float ContinentAmplitude = 30000.f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float MountainRangeFrequency = 0.000008f;

    /** Range mask cutoff. Higher = fewer, more isolated mountain ranges. */
    UPROPERTY(EditAnywhere, Category = "Terrain|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MountainRangeThreshold = 0.45f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float MountainFrequency = 0.00003f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float MountainAmplitude = 65000.f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float HillFrequency = 0.0002f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float HillAmplitude = 4000.f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float DetailFrequency = 0.001f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float DetailAmplitude = 500.f;

    /** Domain warp bends everything sideways, killing the "grid of blobs" look. 0 disables it. */
    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float WarpStrength = 9000.f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Shape")
    float WarpFrequency = 0.00002f;

    // --------------------------------------------------------------- Biomes

    UPROPERTY(EditAnywhere, Category = "Terrain|Biomes")
    TArray<FTerrainBiome> Biomes;

    /**
     * CHANGED from 0.0000012 / 0.0000017. The old values gave climate cells
     * roughly 800 km across, so every transition spanned tens of kilometres --
     * a large part of why everything read as one averaged mush. These give
     * cells on the order of 50-80 km. Lower them again if you want continental
     * climate bands rather than regional ones.
     */
    UPROPERTY(EditAnywhere, Category = "Terrain|Biomes")
    float TemperatureFrequency = 0.000012f;

    UPROPERTY(EditAnywhere, Category = "Terrain|Biomes")
    float MoistureFrequency = 0.000017f;

    /** Higher = tighter biomes with narrower transition bands. */
    UPROPERTY(EditAnywhere, Category = "Terrain|Biomes", meta = (ClampMin = "1.0"))
    float BiomeBlendSharpness = 24.f;

    /**
     * FBM output is bell-distributed around zero rather than uniform over -1..1,
     * because summing octaves pushes the result toward a Gaussian. Raw climate
     * therefore clusters near 0.5 and biomes parked at the corners of climate
     * space (Desert at T=0.94, Tundra at T=0.08) can be unreachable.
     *
     * This widens the distribution. 1 = raw noise, unchanged. Run
     * LogBiomeCoverage first -- if every biome already shows up, leave it at 1.
     */
    UPROPERTY(EditAnywhere, Category = "Terrain|Biomes", meta = (ClampMin = "1.0", ClampMax = "8.0"))
    float ClimateContrast = 1.f;

    /**
     * Exponent applied to biome weights after the floor subtraction. 1 = linear,
     * higher = the dominant biome takes over faster. 1.5 - 3 is the useful range.
     */
    UPROPERTY(EditAnywhere, Category = "Terrain|Biomes", meta = (ClampMin = "1.0", ClampMax = "8.0"))
    float BiomeContrast = 2.f;

    // ------------------------------------------------------------------ API

    /** Thread-safe. Full-detail height at a world XY, usable for gameplay queries. */
    UFUNCTION(BlueprintCallable, Category = "Terrain")
    float GetHeight(float WorldX, float WorldY) const;

    /**
     * Thread-safe. Normalised surface layer weights at a world XY, in the same
     * RGBA order the material sees. Handy for footstep sounds, decals, and
     * scatter/foliage placement -- ask the terrain what it's made of here.
     */
    UFUNCTION(BlueprintCallable, Category = "Terrain")
    FLinearColor GetSurfaceLayerWeights(float WorldX, float WorldY) const;

    /** Thread-safe. Name of the strongest-weighted biome at a world XY. */
    UFUNCTION(BlueprintCallable, Category = "Terrain")
    FName GetBiomeAt(float WorldX, float WorldY) const;

    /**
     * Diagnostic. Samples a grid and logs how much of it each biome dominates.
     * A biome at 0% is unreachable -- its climate coordinates sit outside the
     * range the noise actually produces, so no amount of blend tuning will
     * surface it. Raise ClimateContrast or move that biome inward.
     */
    UFUNCTION(BlueprintCallable, CallInEditor, Category = "Terrain")
    void LogBiomeCoverage(int32 GridSamples = 96, float WorldExtent = 4000000.f) const;

    /** Wipes every chunk and rebuilds from scratch. Pass 0 to keep the current seed. */
    UFUNCTION(BlueprintCallable, Category = "Terrain")
    void Regenerate(int32 NewSeed = 0);

private:

    // ------------------------------------------------------------- Internal

    void InitializeNoise();
    void UpdateStreaming();
    void DispatchPendingJobs();
    void IntegrateCompletedChunks();

    void BuildChunkMesh(FIntPoint Coord, int32 LOD, const FIntVector4& NeighborLODs,
                        UE::Geometry::FDynamicMesh3& OutMesh) const;
    void ApplyChunk(FTerrainChunkResult& Result);

    /**
     * Core terrain evaluator. Purely a function of world XY -- no LOD term, which is
     * what guarantees neighbouring chunks agree exactly on shared vertex positions.
     * Const and free of mutable state, so worker threads can call it concurrently.
     *
     * OutLayerWeights, if supplied, must point at NumSurfaceLayers floats. They come
     * back normalised to sum to 1.
     */
    void SampleTerrain(double WorldX, double WorldY,
                       float& OutHeight, float* OutLayerWeights,
                       int32* OutDominantBiome = nullptr) const;

    UDynamicMeshComponent* AcquireMeshComponent();
    void ReleaseMeshComponent(UDynamicMeshComponent* Comp);

    FVector GetFocusLocation() const;
    FIntPoint GetFocusChunk() const;
    int32 ComputeLOD(float ChunkDistance) const;
    FIntVector4 ComputeNeighborLODs(FIntPoint Coord, FIntPoint Center) const;

    struct FPendingChunk
    {
        FIntPoint Coord;
        int32 LOD;
        FIntVector4 NeighborLODs;
        float Distance;
    };

    UPROPERTY()
    TMap<FIntPoint, FTerrainChunk> Chunks;

    UPROPERTY()
    TArray<TObjectPtr<UDynamicMeshComponent>> ComponentPool;

    // Named fields beat a generic array here: each has a distinct role and we
    // avoid an indirection + bounds check per sample.
    UPROPERTY(Transient) TObjectPtr<UFastNoiseWrapper> NoiseContinent = nullptr;
    UPROPERTY(Transient) TObjectPtr<UFastNoiseWrapper> NoiseRange = nullptr;
    UPROPERTY(Transient) TObjectPtr<UFastNoiseWrapper> NoiseMountain = nullptr;
    UPROPERTY(Transient) TObjectPtr<UFastNoiseWrapper> NoiseHills = nullptr;
    UPROPERTY(Transient) TObjectPtr<UFastNoiseWrapper> NoiseDetail = nullptr;
    UPROPERTY(Transient) TObjectPtr<UFastNoiseWrapper> NoiseWarp = nullptr;
    UPROPERTY(Transient) TObjectPtr<UFastNoiseWrapper> NoiseTemperature = nullptr;
    UPROPERTY(Transient) TObjectPtr<UFastNoiseWrapper> NoiseMoisture = nullptr;

    TArray<FPendingChunk> PendingQueue;
    TSet<FIntPoint> InFlight;
    TQueue<TUniquePtr<FTerrainChunkResult>, EQueueMode::Mpsc> CompletedChunks;

    FThreadSafeCounter ActiveJobs;
    std::atomic<bool> bShuttingDown{ false };

    FIntPoint LastFocusChunk = FIntPoint(MIN_int32, MIN_int32);
    bool bNoiseReady = false;
};
