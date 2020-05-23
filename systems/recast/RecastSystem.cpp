#include <GL/glew.h>
#include <GL/GL.h>

#include "EntityManager.hpp"

#include "RecastSystem.hpp"
#include "RecastComponent.hpp"
#include "RecastDebugShader.hpp"

#include "data/NavMeshComponent.hpp"
#include "data/AdjustableComponent.hpp"
#include "data/ModelDataComponent.hpp"
#include "functions/Execute.hpp"

#include "helpers/assertHelper.hpp"

#include "angle.hpp"
#include "with.hpp"

#include "data/ShaderComponent.hpp"

#include "data/ModelComponent.hpp"

namespace Flags {
	enum {
		Walk = 1,
	};
}

namespace kengine {
	static EntityManager * g_em;

	// declarations
	static void execute(float deltaTime);
	//
	EntityCreator * RecastSystem(EntityManager & em) {
		g_em = &em;

		return [](Entity & e) {
			e += functions::Execute{ execute };
			e += makeGBufferShaderComponent<RecastDebugShader>(*g_em);
		};
	}

	// declarations
	static void buildNavMeshes();
	//
	static void execute(float deltaTime) {
		buildNavMeshes();
	}

	// declarations
	static void createRecastMesh(RecastComponent::Mesh & recast, const NavMeshComponent & navMesh, const ModelDataComponent & modelData, const ModelDataComponent::Mesh & meshData);
	//
	static void buildNavMeshes() {
		static const auto buildRecastComponent = [](auto && entities) {
			for (auto & [e, modelData, navMesh, _] : entities) {
				g_em->runTask([&] {
					kengine_assert(*g_em, navMesh.vertsPerPoly <= DT_VERTS_PER_POLYGON);
					auto & comp = e.attach<RecastComponent>();
					for (const auto & mesh : modelData.meshes) {
						comp.meshes.emplace_back();
						createRecastMesh(comp.meshes.back(), navMesh, modelData, mesh);
					}
					if constexpr (std::is_same<RebuildNavMeshComponent, putils_typeof(_)>())
						e.detach<RebuildNavMeshComponent>();
				});
			}
			g_em->completeTasks();
		};

		buildRecastComponent(g_em->getEntities<ModelDataComponent, NavMeshComponent, no<RecastComponent>>());
		buildRecastComponent(g_em->getEntities<ModelDataComponent, NavMeshComponent, RebuildNavMeshComponent>());
	}

	// declarations
	using HeightfieldPtr = UniquePtr<rcHeightfield, rcFreeHeightField>;
	using CompactHeightfieldPtr = UniquePtr<rcCompactHeightfield, rcFreeCompactHeightfield>;
	using ContourSetPtr = UniquePtr<rcContourSet, rcFreeContourSet>;
	using PolyMeshPtr = UniquePtr<rcPolyMesh, rcFreePolyMesh>;
	using PolyMeshDetailPtr = UniquePtr<rcPolyMeshDetail, rcFreePolyMeshDetail>;

	std::unique_ptr<float[]> getVertices(const ModelDataComponent & modelData, const ModelDataComponent::Mesh & meshData);
	static rcConfig getConfig(const NavMeshComponent & navMesh, const ModelDataComponent::Mesh & meshData, const float * vertices);
	static HeightfieldPtr createHeightField(rcContext & ctx, const rcConfig & cfg, const kengine::ModelDataComponent::Mesh & meshData, const float * vertices);
	static CompactHeightfieldPtr createCompactHeightField(rcContext & ctx, const rcConfig & cfg, rcHeightfield & heightField);
	static ContourSetPtr createContourSet(rcContext & ctx, const rcConfig & cfg, rcCompactHeightfield & chf);
	static PolyMeshPtr createPolyMesh(rcContext & ctx, const rcConfig & cfg, rcContourSet & contourSet);
	static PolyMeshDetailPtr createPolyMeshDetail(rcContext & ctx, const rcConfig & cfg, const rcPolyMesh & polyMesh, const rcCompactHeightfield & chf);
	static NavMeshPtr createNavMesh(const rcConfig & cfg, const rcPolyMesh & polyMesh, const rcPolyMeshDetail & polyMeshDetail);
	static NavMeshQueryPtr createNavMeshQuery(const NavMeshComponent & params, const dtNavMesh & navMesh);
	//
	static void createRecastMesh(RecastComponent::Mesh & recast, const NavMeshComponent & navMesh, const ModelDataComponent & modelData, const ModelDataComponent::Mesh & meshData) {
		const auto vertices = getVertices(modelData, meshData);

		const auto cfg = getConfig(navMesh, meshData, vertices.get());
		if (cfg.width == 0 || cfg.height == 0) {
			kengine_assert_failed(*g_em, "[Recast] Mesh was 0 height or width?");
			return;
		}

		rcContext ctx;
		ctx.resetTimers();
		ctx.startTimer(RC_TIMER_TOTAL);

		const auto heightField = createHeightField(ctx, cfg, meshData, vertices.get());
		if (heightField == nullptr)
			return;

		rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *heightField);
		rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *heightField);
		rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *heightField);

		const auto compactHeightField = createCompactHeightField(ctx, cfg, *heightField);
		if (compactHeightField == nullptr)
			return;

		const auto contourSet = createContourSet(ctx, cfg, *compactHeightField);
		if (contourSet == nullptr)
			return;

		const auto polyMesh = createPolyMesh(ctx, cfg, *contourSet);
		if (polyMesh == nullptr)
			return;

		const auto polyMeshDetail = createPolyMeshDetail(ctx, cfg, *polyMesh, *compactHeightField);
		if (polyMeshDetail == nullptr)
			return;

		recast.navMesh = createNavMesh(cfg, *polyMesh, *polyMeshDetail);
		if (recast.navMesh == nullptr)
			return;

		recast.navMeshQuery = createNavMeshQuery(navMesh, *recast.navMesh);
		if (recast.navMeshQuery == nullptr)
			return;

		ctx.stopTimer(RC_TIMER_TOTAL);
	}

	// declarations
	const std::ptrdiff_t getVertexPositionOffset(const ModelDataComponent & modelData);
	const float * getVertexPosition(const void * vertices, size_t index, size_t vertexSize, std::ptrdiff_t positionOffset);
	//
	std::unique_ptr<float[]> getVertices(const ModelDataComponent & modelData, const ModelDataComponent::Mesh & meshData) {
		const auto vertexSize = modelData.getVertexSize();
		const auto positionOffset = getVertexPositionOffset(modelData);
		if (positionOffset == -1)
			return nullptr;

		auto vertices = std::unique_ptr<float[]>(new float[meshData.vertices.nbElements * 3]);

		for (size_t vertex = 0; vertex < meshData.vertices.nbElements; ++vertex) {
			const auto pos = getVertexPosition(meshData.vertices.data, vertex, vertexSize, positionOffset);
			for (size_t i = 0; i < 3; ++i)
				vertices[vertex * 3 + i] = pos[i];
		}

		return vertices;
	}

	const std::ptrdiff_t getVertexPositionOffset(const ModelDataComponent & modelData) {
		static const char * potentialNames[] = { "pos", "position" };

		for (const auto name : potentialNames) {
			const auto offset = modelData.getVertexAttributeOffset(name);
			if (offset >= 0)
				return offset;
		}

		kengine_assert_failed(*g_em, "[Recast] Could not find vertex position");
		return -1;
	}

	const float * getVertexPosition(const void * vertices, size_t index, size_t vertexSize, std::ptrdiff_t positionOffset) {
		const auto vertex = (const char *)vertices + index * vertexSize;
		return (const float *)(vertex + positionOffset);
	}

	static rcConfig getConfig(const NavMeshComponent & navMesh, const ModelDataComponent::Mesh & meshData, const float * vertices) {
		rcConfig cfg;
		memset(&cfg, 0, sizeof(cfg));

		{ putils_with(navMesh) {
			cfg.cs = _.cellSize;
			kengine_assert(*g_em, cfg.cs > 0);

			cfg.ch = _.cellHeight;
			kengine_assert(*g_em, cfg.ch > 0);

			cfg.walkableSlopeAngle = putils::toDegrees(_.walkableSlope);
			kengine_assert(*g_em, cfg.walkableSlopeAngle > 0.f && cfg.walkableSlopeAngle <= 90.f);

			cfg.walkableHeight = (int)ceilf(_.characterHeight / _.cellHeight);
			kengine_assert(*g_em, cfg.walkableHeight >= 3);

			cfg.walkableClimb = (int)floorf(_.characterClimb / _.cellHeight);
			kengine_assert(*g_em, cfg.walkableClimb >= 0);

			cfg.walkableRadius = (int)ceilf(_.characterRadius / _.cellSize);
			kengine_assert(*g_em, cfg.walkableRadius >= 0);

			cfg.maxEdgeLen = (int)(_.maxEdgeLength / _.cellSize);
			kengine_assert(*g_em, cfg.maxEdgeLen >= 0);

			cfg.maxSimplificationError = _.maxSimplificationError;
			kengine_assert(*g_em, cfg.maxSimplificationError >= 0);

			cfg.minRegionArea = (int)rcSqr(_.minRegionArea);
			kengine_assert(*g_em, cfg.minRegionArea >= 0);

			cfg.mergeRegionArea = (int)rcSqr(_.mergeRegionArea);
			kengine_assert(*g_em, cfg.mergeRegionArea >= 0);

			cfg.maxVertsPerPoly = _.vertsPerPoly;
			kengine_assert(*g_em, cfg.maxVertsPerPoly >= 3);

			cfg.detailSampleDist = _.detailSampleDist;
			kengine_assert(*g_em, cfg.detailSampleDist == 0.f || cfg.detailSampleDist >= .9f);

			cfg.detailSampleMaxError = _.detailSampleMaxError;
			kengine_assert(*g_em, cfg.detailSampleMaxError >= 0.f);
		} }

		rcCalcBounds(vertices, (int)meshData.vertices.nbElements, cfg.bmin, cfg.bmax);
		rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

		return cfg;
	}

	static HeightfieldPtr createHeightField(rcContext & ctx, const rcConfig & cfg, const kengine::ModelDataComponent::Mesh & meshData, const float * vertices) {
		HeightfieldPtr heightField{ rcAllocHeightfield() };

		if (heightField == nullptr) {
			kengine_assert_failed(*g_em, "[Recast] Failed to allocate height field");
			return nullptr;
		}

		if (!rcCreateHeightfield(&ctx, *heightField, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to create height field");
			return nullptr;
		}

		const auto nbTriangles = meshData.indices.nbElements / 3; // I think?
		const auto triangleAreas = new unsigned char[nbTriangles];
		memset(triangleAreas, 0, nbTriangles);

		int * indices = (int *)meshData.indices.data;
		bool mustDeleteIndices = false;
		if (meshData.indexType == GL_UNSIGNED_INT) {
			indices = new int[meshData.indices.nbElements];
			mustDeleteIndices = true;
			const auto unsignedIndices = (const unsigned int *)meshData.indices.data;
			for (int i = 0; i < meshData.indices.nbElements; ++i)
				indices[i] = (int)unsignedIndices[i];
		}

		rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle,
			vertices, (int)meshData.vertices.nbElements,
			indices, (int)nbTriangles,
			triangleAreas);

		if (!rcRasterizeTriangles(&ctx, vertices, (int)meshData.vertices.nbElements, indices, triangleAreas, (int)nbTriangles, *heightField, cfg.walkableClimb)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to rasterize triangles");
			delete[] triangleAreas;
			return nullptr;
		}

		if (mustDeleteIndices)
			delete[] indices;

		delete[] triangleAreas;

		return heightField;
	}

	static CompactHeightfieldPtr createCompactHeightField(rcContext & ctx, const rcConfig & cfg, rcHeightfield & heightField) {
		CompactHeightfieldPtr compactHeightField{ rcAllocCompactHeightfield() };

		if (compactHeightField == nullptr) {
			kengine_assert_failed(*g_em, "[Recast] Failed to allocate compact height field");
			return nullptr;
		}

		if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, heightField, *compactHeightField)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to build compact height field");
			return nullptr;
		}

		if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *compactHeightField)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to erode walkable area");
			return nullptr;
		}
		
		// Classic recast positiong. For others, see https://github.com/recastnavigation/recastnavigation/blob/master/RecastDemo/Source/Sample_SoloMesh.cpp
		if (!rcBuildDistanceField(&ctx, *compactHeightField)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to build distance field");
			return nullptr;
		}

		if (!rcBuildRegions(&ctx, *compactHeightField, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to build regions");
			return nullptr;
		}

		return compactHeightField;
	}

	static ContourSetPtr createContourSet(rcContext & ctx, const rcConfig & cfg, rcCompactHeightfield & chf) {
		ContourSetPtr contourSet{ rcAllocContourSet() };

		if (contourSet == nullptr) {
			kengine_assert_failed(*g_em, "[Recast] Failed to allocate contour set");
			return nullptr;
		}

		if (!rcBuildContours(&ctx, chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *contourSet)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to build contours");
			return nullptr;
		}

		return contourSet;
	}

	static PolyMeshPtr createPolyMesh(rcContext & ctx, const rcConfig & cfg, rcContourSet & contourSet) {
		PolyMeshPtr polyMesh{ rcAllocPolyMesh() };

		if (polyMesh == nullptr) {
			kengine_assert_failed(*g_em, "[Recast] Failed to allocate poly mesh");
			return nullptr;
		}
		
		if (!rcBuildPolyMesh(&ctx, contourSet, cfg.maxVertsPerPoly, *polyMesh)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to build poly mesh");
			return nullptr;
		}

		return polyMesh;
	}

	static PolyMeshDetailPtr createPolyMeshDetail(rcContext & ctx, const rcConfig & cfg, const rcPolyMesh & polyMesh, const rcCompactHeightfield & chf) {
		PolyMeshDetailPtr polyMeshDetail{ rcAllocPolyMeshDetail() };
		if (polyMeshDetail == nullptr) {
			kengine_assert_failed(*g_em, "[Recast] Failed to allocate poly mesh detail");
			return nullptr;
		}

		if (!rcBuildPolyMeshDetail(&ctx, polyMesh, chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *polyMeshDetail)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to build poly mesh detail");
			return nullptr;
		}

		return polyMeshDetail;
	}

	static NavMeshPtr createNavMesh(const rcConfig & cfg, const rcPolyMesh & polyMesh, const rcPolyMeshDetail & polyMeshDetail) {
		dtNavMeshCreateParams params;
		memset(&params, 0, sizeof(params));
		{ putils_with(polyMesh) {
			params.verts = _.verts;
			params.vertCount = _.nverts;
			params.polys = _.polys;
			params.polyAreas = _.areas;
			params.polyFlags = _.flags;
			params.polyCount = _.npolys;
			params.nvp = _.nvp;
		} }

		{ putils_with(polyMeshDetail) {
			params.detailMeshes = _.meshes;
			params.detailVerts = _.verts;
			params.detailVertsCount = _.nverts;
			params.detailTris = _.tris;
			params.detailTriCount = _.ntris;
		} }

		params.walkableHeight = (float)cfg.walkableHeight;
		params.walkableClimb = (float)cfg.walkableClimb;
		params.walkableRadius = (float)cfg.walkableRadius;
		rcVcopy(params.bmin, cfg.bmin);
		rcVcopy(params.bmax, cfg.bmax);
		params.cs = cfg.cs;
		params.ch = cfg.ch;

		unsigned char * navMeshData = nullptr;
		int navMeshDataSize;
		if (!dtCreateNavMeshData(&params, &navMeshData, &navMeshDataSize)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to create Detour navmesh data");
			return nullptr;
		}

		NavMeshPtr navMesh{ dtAllocNavMesh() };
		if (navMesh == nullptr) {
			dtFree(navMeshData);
			kengine_assert_failed(*g_em, "[Recast] Failed to allocate Detour navmesh");
			return nullptr;
		}

		const auto status = navMesh->init(navMeshData, navMeshDataSize, DT_TILE_FREE_DATA);
		if (dtStatusFailed(status)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to init Detour navmesh");
			return nullptr;
		}

		return navMesh;
	}

	static NavMeshQueryPtr createNavMeshQuery(const NavMeshComponent & params, const dtNavMesh & navMesh) {
		NavMeshQueryPtr navMeshQuery{ dtAllocNavMeshQuery() };

		if (navMeshQuery == nullptr) {
			kengine_assert_failed(*g_em, "[Recast] Failed to allocate Detour navmesh query");
			return nullptr;
		}

		const auto maxNodes = params.queryMaxSearchNodes;
		kengine_assert(*g_em, 0 < maxNodes && maxNodes <= 65535);
		const auto status = navMeshQuery->init(&navMesh, maxNodes);
		if (dtStatusFailed(status)) {
			kengine_assert_failed(*g_em, "[Recast] Failed to init Detour navmesh query");
			return nullptr;
		}

		return navMeshQuery;
	}
}