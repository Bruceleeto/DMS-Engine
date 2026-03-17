#include "main.h"
#define CGLTF_IMPLEMENTATION
#include "include/cgltf.h"
#include <algorithm>
#include <numeric>
#include <random>
#include <omp.h>

// Transform data
typedef struct Transform {
  Vector3 translation;
  Quaternion rotation;
  Vector3 scale;
} Transform;

typedef struct {
  char name[64];
  int parent;
  Transform bindPose;
  Transform localPose;
  Matrix worldPose;
  Matrix inverseBindMatrix;
} Bone;

typedef struct {
  char name[32];
  int boneCount;
  int frameCount;
  float duration;
  Transform **framePoses;
} Animation;

typedef struct {
  Bone *bones;
  int boneCount;
  Animation *animations;
  int animCount;
  int currentAnim;
  float currentTime;
} Skeleton;

typedef struct {
  float x, y, z;      // 12
  float u, v;         // 8
  uint8_t b, g, r, a; // 4 - BGRA order
  uint8_t boneId;     // 1
  int8_t nx, ny, nz;  // 3
  float boneWeight;   // 4
} Vertex;             // 32 bytes

typedef struct {
  Vertex *vertices;         // Dynamic vertex array
  Vertex *originalVertices; // Store the original vertex positions
  Vertex *animatedVertices; // Working buffer for animations

  unsigned int *indices;      // Dynamic index array
  unsigned int *stripLengths; // Array of strip lengths
  int stripCount;             // Number of strips
  int looseIndexCount;        // Number of indices in loose triangles
  int vertexCount;
  int indexCount;
  int textureId;
  uint32_t materialColor;

  // Material flags (Phase 0.2)
  int alphaMode;          // 0=OPAQUE, 1=CUTOUT, 2=TRANSPARENT
  float alphaCutoff;      // for CUTOUT mode (default 0.5)
  int doubleSided;        // 0=single-sided, 1=double-sided
  int wrapU, wrapV;       // raw glTF sampler values (10497=REPEAT, 33071=CLAMP, 33648=MIRROR)

} Mesh;

typedef struct {
  Mesh *meshes;
  int meshCount;
  Skeleton *skeleton;
} Model;

typedef struct {
  float x, y, z;      // 12
  float u, v;         // 8
  uint32_t argb;      // 4
  int8_t nx, ny, nz;  // 3
  uint8_t pad;        // 1
  uint32_t flags;     // 4
} StaticVertex;       // 32 bytes

struct Vertex_Tristripped {
  float position[3];
  float normal[3];
  float texcoord[2];
  uint32_t color;
  uint32_t vertexId;
  uint32_t normalId;
};

struct Triangle {
  Vertex_Tristripped vertices[3];
  uint32_t materialId;
};

std::vector<Triangle> triangles;
std::vector<std::vector<size_t>>
join_strips(const triangle_stripper::primitive_vector &originalStrips);
std::vector<std::vector<uint32_t>> g_raw_strips;
std::vector<uint32_t> g_loose_triangles;
std::vector<std::array<float, 9>> g_rtTriangles; // v0, v1, v2 packed

bool LoadGLTF(const char *filename);
void Cleanup(void);
void optimize_mesh();
bool can_join_strips(const std::vector<size_t> &strip1,
                     const std::vector<size_t> &strip2);
void ExportTristrippedModel(const Model *model, const char *filename,
                            bool bakeLighting);

#define POSITION_THRESHOLD 0.001f
#define ROTATION_THRESHOLD 0.001f
#define SCALE_THRESHOLD 0.001f

Skeleton skeleton = {0};
Model model = {0};
Model tristrippedModel = {0};            // Second model for tristripped version
std::map<int, std::string> textureNames; // Track texture ID to filename

// Write matrix in column-major order for GL
void WriteMatrixColumnMajor(FILE *file, const Matrix &m) {
  float col_major[16] = {
      m.m0,  m.m1,  m.m2,  m.m3,  // Column 0
      m.m4,  m.m5,  m.m6,  m.m7,  // Column 1
      m.m8,  m.m9,  m.m10, m.m11, // Column 2
      m.m12, m.m13, m.m14, m.m15  // Column 3
  };
  fwrite(col_major, sizeof(float), 16, file);
}

// Write Transform with quaternion in WXYZ order for GL
void WriteDMSTransform(FILE *file, const Transform &t) {
  fwrite(&t.translation, sizeof(Vector3), 1, file);
  float wxyz[4] = {t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z};
  fwrite(wxyz, sizeof(float), 4, file);
  fwrite(&t.scale, sizeof(Vector3), 1, file);
}

// Find joint index in skeleton
static int GetNodeBoneIndex(const cgltf_node *node, const cgltf_skin *skin) {
  if (!node || !skin)
    return -1;

  for (size_t i = 0; i < skin->joints_count; i++) {
    if (skin->joints[i] == node)
      return (int)i;
  }
  return -1;
}

float QuaternionDotProduct(Quaternion a, Quaternion b) {
  return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// Get transform from node
static Transform GetNodeTransform(const cgltf_node *node) {
  Transform transform = {.translation = {0},
                         .rotation = QuaternionIdentity(),
                         .scale = {1.0f, 1.0f, 1.0f}};

  if (node->has_matrix) {
    // Lay out glTF's row-major matrix (node->matrix[16]) into
    // a raylib Matrix (column-major), the same way cgltf_node_transform_world()
    // does.
    //
    Matrix mat = {
        node->matrix[0], node->matrix[4], node->matrix[8],  node->matrix[12],
        node->matrix[1], node->matrix[5], node->matrix[9],  node->matrix[13],
        node->matrix[2], node->matrix[6], node->matrix[10], node->matrix[14],
        node->matrix[3], node->matrix[7], node->matrix[11], node->matrix[15]};

    // Extract T, R, S from that matrix directly
    Vector3 t, s;
    Quaternion r;
    MatrixDecompose(mat, &t, &r, &s);

    transform.translation = t;
    transform.rotation = r;
    transform.scale = s;
  } else {
    // If no matrix, fall back to node->translation / node->rotation /
    // node->scale
    if (node->has_translation) {
      transform.translation = (Vector3){
          node->translation[0], node->translation[1], node->translation[2]};
    }
    if (node->has_rotation) {
      transform.rotation = (Quaternion){node->rotation[0], node->rotation[1],
                                        node->rotation[2], node->rotation[3]};
    }
    if (node->has_scale) {
      transform.scale =
          (Vector3){node->scale[0], node->scale[1], node->scale[2]};
    }
  }

  return transform;
}

struct StripInfo {
  std::vector<uint32_t> indices; // Indices making up this strip
  uint32_t stripId;              // ID of this strip
};

struct MeshTriStrips {
  std::vector<Vertex> vertices;         // Optimized vertex buffer
  std::vector<StripInfo> strips;        // List of strips
  std::vector<uint32_t> looseTriangles; // Non-stripped triangles
  std::map<uint32_t, uint32_t>
      vertexMap; // Original to optimized vertex mapping
  int textureId; //   Texture ID for this mesh
};

bool isKeyframeNeeded(const Transform &current, const Transform &last) {
  // Check translation change
  Vector3 posDelta = Vector3Subtract(current.translation, last.translation);
  if (Vector3Length(posDelta) > POSITION_THRESHOLD) {
    return true;
  }

  // Check rotation change using dot product
  float rotDelta =
      1.0f - fabsf(QuaternionDotProduct(current.rotation, last.rotation));
  if (rotDelta > ROTATION_THRESHOLD) {
    return true;
  }

  // Check scale change
  Vector3 scaleDelta = Vector3Subtract(current.scale, last.scale);
  if (Vector3Length(scaleDelta) > SCALE_THRESHOLD) {
    return true;
  }

  return false;
}

// Returns how many keyframes were actually needed
int reduceKeyframes(Transform *poses, int frameCount, int boneCount) {
  std::vector<Transform> optimizedPoses;
  optimizedPoses.reserve(frameCount * boneCount);

  // Always keep first frame
  for (int i = 0; i < boneCount; i++) {
    optimizedPoses.push_back(poses[i]);
  }

  // Check each subsequent frame
  for (int frame = 1; frame < frameCount; frame++) {
    bool frameNeeded = false;
    int poseOffset = frame * boneCount;

    // Check if any bone has significant change
    for (int bone = 0; bone < boneCount; bone++) {
      Transform &current = poses[poseOffset + bone];
      Transform &last =
          optimizedPoses[optimizedPoses.size() - boneCount + bone];

      if (isKeyframeNeeded(current, last)) {
        frameNeeded = true;
        break;
      }
    }

    // If frame needed, copy all bone transforms
    if (frameNeeded) {
      for (int bone = 0; bone < boneCount; bone++) {
        optimizedPoses.push_back(poses[poseOffset + bone]);
      }
    }
  }

  // Always keep last frame if it wasn't already kept
  int lastFrame = (frameCount - 1) * boneCount;
  if (optimizedPoses.size() < (lastFrame + boneCount)) {
    for (int bone = 0; bone < boneCount; bone++) {
      optimizedPoses.push_back(poses[lastFrame + bone]);
    }
  }

  // Copy back to original array
  memcpy(poses, optimizedPoses.data(),
         optimizedPoses.size() * sizeof(Transform));

  return optimizedPoses.size() / boneCount;
}

MeshTriStrips ExtractTriStrips(const Mesh *srcMesh) {
  MeshTriStrips result;
  result.textureId = srcMesh->textureId; //  Copy texture ID

  std::map<std::string, uint32_t> tempVertexMap;

  triangles.clear();

  if (srcMesh->indexCount > 0 && srcMesh->indices) {
    // Indexed mesh - process triangles using indices
    for (int i = 0; i < srcMesh->indexCount; i += 3) {
      Triangle tri;
      tri.materialId = 0;

      for (int j = 0; j < 3; j++) {
        int idx = srcMesh->indices[i + j];
        const Vertex &v = srcMesh->vertices[idx];

        // Create vertex key - Updated for single bone
        char key[256];
        snprintf(key, sizeof(key), "%.6f,%.6f,%.6f|%.6f,%.6f|%d,%d,%d|%u,%f",
                 v.x, v.y, v.z, v.u, v.v, (int)v.nx, (int)v.ny, (int)v.nz,
                 (unsigned int)v.boneId, v.boneWeight);

        // Look up or add vertex
        auto it = tempVertexMap.find(key);
        uint32_t vertex_idx;
        if (it == tempVertexMap.end()) {
          vertex_idx = result.vertices.size();
          tempVertexMap[key] = vertex_idx;
          result.vertices.push_back(v);
          result.vertexMap[idx] = vertex_idx;
        } else {
          vertex_idx = it->second;
        }

        // Setup triangle data
        Vertex_Tristripped tv;
        memcpy(tv.position, &v.x, sizeof(float) * 3);
        memcpy(tv.normal, &v.nx, sizeof(float) * 3);
        tv.texcoord[0] = v.u;
        tv.texcoord[1] = v.v;
        tv.color = 0xFFFFFFFF;
        tv.vertexId = vertex_idx;
        tv.normalId = vertex_idx;

        tri.vertices[j] = tv;
      }
      triangles.push_back(tri);
    }
  } else if (srcMesh->vertexCount > 0) {
    // Non-indexed mesh - process vertices directly as triangles
    for (int i = 0; i < srcMesh->vertexCount; i += 3) {
      Triangle tri;
      tri.materialId = 0;

      for (int j = 0; j < 3; j++) {
        const Vertex &v = srcMesh->vertices[i + j];

        // Create vertex key - Updated for single bone
        char key[256]; // Reduced size since   need less space now
        snprintf(key, sizeof(key),
                 "%.6f,%.6f,%.6f|%.6f,%.6f|%.6f,%.6f,%.6f|%u,%f", v.x, v.y, v.z,
                 v.u, v.v, (float)v.nx / 127.0f, (float)v.ny / 127.0f,
                 (float)v.nz / 127.0f, (unsigned int)v.boneId, v.boneWeight);

        // Look up or add vertex
        auto it = tempVertexMap.find(key);
        uint32_t vertex_idx;
        if (it == tempVertexMap.end()) {
          vertex_idx = result.vertices.size();
          tempVertexMap[key] = vertex_idx;
          result.vertices.push_back(v);
          result.vertexMap[i + j] = vertex_idx;
        } else {
          vertex_idx = it->second;
        }

        // Setup triangle data
        Vertex_Tristripped tv;
        memcpy(tv.position, &v.x, sizeof(float) * 3);
        memcpy(tv.normal, &v.nx, sizeof(float) * 3);
        tv.texcoord[0] = v.u;
        tv.texcoord[1] = v.v;
        tv.color = 0xFFFFFFFF;
        tv.vertexId = vertex_idx;
        tv.normalId = vertex_idx;

        tri.vertices[j] = tv;
      }
      triangles.push_back(tri);
    }
  }

  if (!triangles.empty()) {
    optimize_mesh();

    for (size_t s = 0; s < g_raw_strips.size(); s++) {
      StripInfo strip;
      strip.stripId = s + 1;
      for (uint32_t idx : g_raw_strips[s]) {
        strip.indices.push_back(idx);
      }
      result.strips.push_back(strip);
    }

    // Use loose triangles from optimizer, not from triangles vector
    for (uint32_t idx : g_loose_triangles) {
      result.looseTriangles.push_back(idx);
    }
  }

  return result;
}


void ComputeBoundingSphere(const Mesh *mesh, float *cx, float *cy, float *cz, float *radius) {
  if (mesh->vertexCount == 0) {
    *cx = *cy = *cz = *radius = 0.0f;
    return;
  }

  // Find AABB center
  float minX = mesh->vertices[0].x, maxX = minX;
  float minY = mesh->vertices[0].y, maxY = minY;
  float minZ = mesh->vertices[0].z, maxZ = minZ;

  for (int i = 1; i < mesh->vertexCount; i++) {
    const Vertex &v = mesh->vertices[i];
    if (v.x < minX) minX = v.x;
    if (v.x > maxX) maxX = v.x;
    if (v.y < minY) minY = v.y;
    if (v.y > maxY) maxY = v.y;
    if (v.z < minZ) minZ = v.z;
    if (v.z > maxZ) maxZ = v.z;
  }

  *cx = (minX + maxX) * 0.5f;
  *cy = (minY + maxY) * 0.5f;
  *cz = (minZ + maxZ) * 0.5f;

  // Find max distance from center
  float maxDistSq = 0.0f;
  for (int i = 0; i < mesh->vertexCount; i++) {
    const Vertex &v = mesh->vertices[i];
    float dx = v.x - *cx;
    float dy = v.y - *cy;
    float dz = v.z - *cz;
    float distSq = dx*dx + dy*dy + dz*dz;
    if (distSq > maxDistSq) maxDistSq = distSq;
  }

  *radius = sqrtf(maxDistSq);
}





/* Read image dimensions from a PNG or JPEG file header. */
static bool GetImageDimensions(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[24];
    if (fread(hdr, 1, 24, f) < 24) { fclose(f); return false; }

    /* PNG: 89 50 4E 47 ... IHDR width(4) height(4) at offset 16 */
    if (hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G') {
        *w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
        *h = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
        fclose(f);
        return true;
    }

    /* JPEG: scan for SOF0 (FFC0) or SOF2 (FFC2) */
    if (hdr[0] == 0xFF && hdr[1] == 0xD8) {
        fseek(f, 2, SEEK_SET);
        uint8_t buf[2];
        while (fread(buf, 1, 2, f) == 2) {
            if (buf[0] != 0xFF) break;
            if (buf[1] == 0xC0 || buf[1] == 0xC2) {
                uint8_t sof[7];
                if (fread(sof, 1, 7, f) < 7) break;
                *h = (sof[3] << 8) | sof[4];
                *w = (sof[5] << 8) | sof[6];
                fclose(f);
                return true;
            }
            /* skip marker payload */
            uint8_t lb[2];
            if (fread(lb, 1, 2, f) < 2) break;
            fseek(f, ((lb[0] << 8) | lb[1]) - 2, SEEK_CUR);
        }
    }

    fclose(f);
    return false;
}

/* Run pvrtex on an image file. Enables mipmaps only for square textures. */
static int RunPvrtex(const char *inPath, const char *outPath) {
    int w = 0, h = 0;
    bool square = GetImageDimensions(inPath, &w, &h) && (w == h);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "/opt/toolchains/dc/kos/utils/pvrtex/pvrtex -i \"%s\" -o \"%s\" -f auto -c%s",
        inPath, outPath, square ? " -m" : "");
    printf("  Running: %s\n", cmd);
    return system(cmd);
}

/* Check if a cgltf image is a PNG with an alpha channel (RGBA or gray+alpha).
 * Works for both embedded (buffer_view) and external images. */
static bool CgltfImageHasAlpha(cgltf_image *img, const char *inputDir) {
    uint8_t hdr[26];

    if (img->buffer_view) {
        const uint8_t *p = (const uint8_t *)img->buffer_view->buffer->data
                           + img->buffer_view->offset;
        if (img->buffer_view->size < 26) return false;
        memcpy(hdr, p, 26);
    } else if (img->uri && strncmp(img->uri, "data:", 5) != 0) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", inputDir, img->uri);
        FILE *f = fopen(path, "rb");
        if (!f) return false;
        size_t n = fread(hdr, 1, 26, f);
        fclose(f);
        if (n < 26) return false;
    } else {
        return false;
    }

    /* PNG signature check, then color type at IHDR offset 25:
     * 4 = grayscale+alpha, 6 = RGBA */
    if (hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G')
        return (hdr[25] == 4 || hdr[25] == 6);

    return false; /* JPEG never has alpha */
}

void ExtractAndConvertTextures(cgltf_data *data, const char *inputFilename) {
    if (data->images_count == 0) return;

    // Get directory of input file
    char outputDir[256] = ".";
    strncpy(outputDir, inputFilename, sizeof(outputDir) - 1);
    char *lastSlash = strrchr(outputDir, '/');
    if (!lastSlash) lastSlash = strrchr(outputDir, '\\');
    if (lastSlash) *lastSlash = '\0';
    else strcpy(outputDir, ".");

    printf("\n=== Texture Extraction ===\n");
    printf("Found %zu images in GLB/GLTF\n", data->images_count);

    for (size_t i = 0; i < data->images_count; i++) {
        cgltf_image *img = &data->images[i];
        const void *imageData = NULL;
        size_t imageSize = 0;
        char tmpPath[512], dtPath[512];

        // Determine file extension from mime type
        const char *ext = ".png";
        if (img->mime_type && strstr(img->mime_type, "jpeg")) ext = ".jpg";
        else if (img->mime_type && strstr(img->mime_type, "jpg")) ext = ".jpg";

        snprintf(tmpPath, sizeof(tmpPath), "%s/_tmp_texture_%zu%s", outputDir, i, ext);
        snprintf(dtPath, sizeof(dtPath), "%s/texture_%zu.dt", outputDir, i);

        if (img->buffer_view) {
            // Embedded in .glb binary chunk
            imageData = (const char *)img->buffer_view->buffer->data
                        + img->buffer_view->offset;
            imageSize = img->buffer_view->size;
        } else if (img->uri && strncmp(img->uri, "data:", 5) != 0) {
            // External file - build path relative to input and pass directly to pvrtex
            snprintf(tmpPath, sizeof(tmpPath), "%s/%s", outputDir, img->uri);
            printf("  Texture %zu: external file %s\n", i, tmpPath);

            int ret = RunPvrtex(tmpPath, dtPath);
            if (ret != 0) printf("  WARNING: pvrtex failed for texture %zu\n", i);
            else printf("  Converted texture %zu -> %s\n", i, dtPath);
            continue;
        } else if (img->uri && strncmp(img->uri, "data:", 5) == 0) {
            printf("  Texture %zu: data URI (base64) - skipping\n", i);
            continue;
        } else {
            printf("  Texture %zu: no image data found\n", i);
            continue;
        }

        // Write embedded image to temp file
        if (imageData && imageSize > 0) {
            FILE *tmp = fopen(tmpPath, "wb");
            if (!tmp) {
                printf("  Failed to write temp file: %s\n", tmpPath);
                continue;
            }
            fwrite(imageData, 1, imageSize, tmp);
            fclose(tmp);

            int ret = RunPvrtex(tmpPath, dtPath);
            if (ret != 0) printf("  WARNING: pvrtex failed for texture %zu\n", i);
            else printf("  Converted texture %zu -> %s\n", i, dtPath);

            // Clean up temp file
            remove(tmpPath);
        }
    }
    printf("=== Texture Extraction Complete ===\n\n");
}

bool LoadGLTF(const char *filename) {
  cgltf_options options = {};
  cgltf_data *data = NULL;
  cgltf_result result = cgltf_parse_file(&options, filename, &data);

  if (result != cgltf_result_success) {
    printf("Failed to parse GLTF file: %s\n", filename);
    return false;
  }

  result = cgltf_load_buffers(&options, data, filename);
  if (result != cgltf_result_success) {
    printf("Failed to load GLTF buffers\n");
    cgltf_free(data);
    return false;
  }

  // Load skeleton
  cgltf_skin *skin = NULL;
  if (data->skins_count > 0) {
    skin = &data->skins[0];
    skeleton.boneCount = (int)skin->joints_count;
    skeleton.bones = (Bone *)calloc(skeleton.boneCount, sizeof(Bone));

    // Load bone data
    for (int i = 0; i < skeleton.boneCount; i++) {
      cgltf_node *node = skin->joints[i];
      Bone *bone = &skeleton.bones[i];

      // Set name
      if (node->name) {
        strncpy(bone->name, node->name, sizeof(bone->name) - 1);
      } else {
        snprintf(bone->name, sizeof(bone->name), "bone_%d", i);
      }

      // Get parent index
      bone->parent = -1;
      if (node->parent) {
        bone->parent = GetNodeBoneIndex(node->parent, skin);
      }

      // Get bind pose transform
      bone->bindPose = GetNodeTransform(node);
      bone->localPose = bone->bindPose;

      // Get inverse bind matrix if available
      bone->inverseBindMatrix = MatrixIdentity();
      if (skin->inverse_bind_matrices) {
        cgltf_accessor *ibmAccessor = skin->inverse_bind_matrices;
        float *matrices =
            (float *)((char *)ibmAccessor->buffer_view->buffer->data +
                      ibmAccessor->buffer_view->offset + ibmAccessor->offset);

        float *m4 = &matrices[i * 16];
        Matrix tempIBM = {m4[0], m4[4],  m4[8],  m4[12], m4[1],  m4[5],
                          m4[9], m4[13], m4[2],  m4[6],  m4[10], m4[14],
                          m4[3], m4[7],  m4[11], m4[15]};
        bone->inverseBindMatrix = tempIBM;
      }
    }

    // Get the armature (skin root) world transform so root bones
    // inherit the armature's scale (e.g. Mixamo 0.01 cm→m).
    Matrix armatureWorld = MatrixIdentity();
    {
      cgltf_node *armatureNode = skin->skeleton;
      if (!armatureNode && skin->joints_count > 0 && skin->joints[0]->parent)
        armatureNode = skin->joints[0]->parent;
      if (armatureNode) {
        float m[16];
        cgltf_node_transform_world(armatureNode, m);
        armatureWorld = (Matrix){m[0], m[4],  m[8],  m[12], m[1],  m[5],
                                 m[9], m[13], m[2],  m[6],  m[10], m[14],
                                 m[3], m[7],  m[11], m[15]};
      }
    }

    // Initialize world poses in correct hierarchy order
    for (int i = 0; i < skeleton.boneCount; i++) {
      Bone *bone = &skeleton.bones[i];

      Matrix translation = MatrixTranslate(bone->bindPose.translation.x,
                                           bone->bindPose.translation.y,
                                           bone->bindPose.translation.z);
      Matrix rotation = QuaternionToMatrix(bone->bindPose.rotation);
      Matrix scale = MatrixScale(bone->bindPose.scale.x, bone->bindPose.scale.y,
                                 bone->bindPose.scale.z);

      Matrix localTransform =
          MatrixMultiply(MatrixMultiply(scale, rotation), translation);

      if (bone->parent < 0) {
        bone->worldPose = MatrixMultiply(localTransform, armatureWorld);
      } else {
        bone->worldPose = MatrixMultiply(
            localTransform, skeleton.bones[bone->parent].worldPose);
      }
    }

    // Extract armature scale to bake into root bone poses
    Vector3 armatureScale = {1.0f, 1.0f, 1.0f};
    {
      cgltf_node *armatureNode = skin->skeleton;
      if (!armatureNode && skin->joints_count > 0 && skin->joints[0]->parent)
        armatureNode = skin->joints[0]->parent;
      if (armatureNode) {
        float m[16];
        cgltf_node_transform_world(armatureNode, m);
        // Extract scale from world transform columns
        armatureScale.x = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
        armatureScale.y = sqrtf(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]);
        armatureScale.z = sqrtf(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
        printf("Armature scale: (%.4f, %.4f, %.4f)\n",
               armatureScale.x, armatureScale.y, armatureScale.z);
      }
    }

    // Bake armature scale into root bone bind poses
    for (int i = 0; i < skeleton.boneCount; i++) {
      Bone *bone = &skeleton.bones[i];
      if (bone->parent < 0) {
        bone->bindPose.translation.x *= armatureScale.x;
        bone->bindPose.translation.y *= armatureScale.y;
        bone->bindPose.translation.z *= armatureScale.z;
        bone->bindPose.scale.x *= armatureScale.x;
        bone->bindPose.scale.y *= armatureScale.y;
        bone->bindPose.scale.z *= armatureScale.z;
      }
    }

    // Load animations if available
    if (data->animations_count > 0) {
      skeleton.animCount = (int)data->animations_count;
      skeleton.animations =
          (Animation *)calloc(skeleton.animCount, sizeof(Animation));

      for (int i = 0; i < skeleton.animCount; i++) {
        cgltf_animation *srcAnim = &data->animations[i];
        Animation *dstAnim = &skeleton.animations[i];

        if (srcAnim->name) {
          strncpy(dstAnim->name, srcAnim->name, sizeof(dstAnim->name) - 1);
        } else {
          snprintf(dstAnim->name, sizeof(dstAnim->name), "anim_%d", i);
        }

        dstAnim->duration = 0.0f;
        for (size_t c = 0; c < srcAnim->channels_count; c++) {
          cgltf_accessor *input = srcAnim->channels[c].sampler->input;
          float *times = (float *)((char *)input->buffer_view->buffer->data +
                                   input->buffer_view->offset + input->offset);
          float lastTime = times[input->count - 1];
          if (lastTime > dstAnim->duration) {
            dstAnim->duration = lastTime;
          }
        }

        dstAnim->frameCount = (int)(dstAnim->duration * 30.0f) + 1;
        dstAnim->boneCount = skeleton.boneCount;

        Transform *poses = (Transform *)calloc(
            dstAnim->frameCount * skeleton.boneCount, sizeof(Transform));
        for (int frame = 0; frame < dstAnim->frameCount; frame++) {
          for (int bone = 0; bone < skeleton.boneCount; bone++) {
            poses[frame * skeleton.boneCount + bone] =
                skeleton.bones[bone].bindPose;
          }
        }

        for (size_t channelId = 0; channelId < srcAnim->channels_count;
             channelId++) {
          cgltf_animation_channel *channel = &srcAnim->channels[channelId];
          int boneIndex = GetNodeBoneIndex(channel->target_node, skin);
          if (boneIndex < 0)
            continue;

          cgltf_animation_sampler *sampler = channel->sampler;
          float *times =
              (float *)((char *)sampler->input->buffer_view->buffer->data +
                        sampler->input->buffer_view->offset +
                        sampler->input->offset);
          float *values =
              (float *)((char *)sampler->output->buffer_view->buffer->data +
                        sampler->output->buffer_view->offset +
                        sampler->output->offset);

          for (int frame = 0; frame < dstAnim->frameCount; frame++) {
            float time = frame * (1.0f / 30.0f);

            int prevKey = 0;
            int nextKey = 0;
            for (size_t k = 0; k < sampler->input->count - 1; k++) {
              if (times[k] <= time && times[k + 1] >= time) {
                prevKey = (int)k;
                nextKey = (int)k + 1;
                break;
              }
            }

            float alpha =
                (time - times[prevKey]) / (times[nextKey] - times[prevKey]);
            if (alpha < 0.0f)
              alpha = 0.0f;
            if (alpha > 1.0f)
              alpha = 1.0f;

            Transform *pose = &poses[frame * skeleton.boneCount + boneIndex];

            switch (channel->target_path) {
            case cgltf_animation_path_type_translation: {
              Vector3 prev = {values[prevKey * 3], values[prevKey * 3 + 1],
                              values[prevKey * 3 + 2]};
              Vector3 next = {values[nextKey * 3], values[nextKey * 3 + 1],
                              values[nextKey * 3 + 2]};
              pose->translation = Vector3Lerp(prev, next, alpha);
            } break;
            case cgltf_animation_path_type_rotation: {
              Quaternion prev = {values[prevKey * 4], values[prevKey * 4 + 1],
                                 values[prevKey * 4 + 2],
                                 values[prevKey * 4 + 3]};
              Quaternion next = {values[nextKey * 4], values[nextKey * 4 + 1],
                                 values[nextKey * 4 + 2],
                                 values[nextKey * 4 + 3]};
              pose->rotation = QuaternionSlerp(prev, next, alpha);
            } break;
            case cgltf_animation_path_type_scale: {
              Vector3 prev = {values[prevKey * 3], values[prevKey * 3 + 1],
                              values[prevKey * 3 + 2]};
              Vector3 next = {values[nextKey * 3], values[nextKey * 3 + 1],
                              values[nextKey * 3 + 2]};
              pose->scale = Vector3Lerp(prev, next, alpha);
            } break;
            default:
              break;
            }
          }
        }

        // Bake armature scale into root bone animation poses
        for (int frame = 0; frame < dstAnim->frameCount; frame++) {
          for (int bone = 0; bone < skeleton.boneCount; bone++) {
            if (skeleton.bones[bone].parent < 0) {
              Transform *pose = &poses[frame * skeleton.boneCount + bone];
              pose->translation.x *= armatureScale.x;
              pose->translation.y *= armatureScale.y;
              pose->translation.z *= armatureScale.z;
              pose->scale.x *= armatureScale.x;
              pose->scale.y *= armatureScale.y;
              pose->scale.z *= armatureScale.z;
            }
          }
        }

        int reducedFrames =
            reduceKeyframes(poses, dstAnim->frameCount, dstAnim->boneCount);
        printf("Animation '%s': Reduced from %d to %d frames\n", dstAnim->name,
               dstAnim->frameCount, reducedFrames);
        dstAnim->frameCount = reducedFrames;
        dstAnim->framePoses = (Transform **)poses;
      }
    }
  } else {
    skeleton.animCount = 0;
    skeleton.animations = NULL;
  }

  // Free existing mesh data if it exists
  if (model.meshes) {
    for (int i = 0; i < model.meshCount; i++) {
      if (model.meshes[i].vertices)
        free(model.meshes[i].vertices);
      if (model.meshes[i].originalVertices)
        free(model.meshes[i].originalVertices);
      if (model.meshes[i].animatedVertices)
        free(model.meshes[i].animatedVertices);
      if (model.meshes[i].indices)
        free(model.meshes[i].indices);
    }
    free(model.meshes);
  }

  model.meshCount = 0;
  model.meshes = NULL;
  model.skeleton = &skeleton;

  // Count total primitives across all meshes
  if (data->meshes_count > 0) {
    int totalPrimitives = 0;
    for (size_t m = 0; m < data->meshes_count; m++) {
      totalPrimitives += (int)data->meshes[m].primitives_count;
    }

    model.meshCount = totalPrimitives;
    model.meshes = (Mesh *)calloc(model.meshCount, sizeof(Mesh));

    int meshIndex = 0;

    for (size_t m = 0; m < data->meshes_count; m++) {
      cgltf_mesh *srcMesh = &data->meshes[m];

      // Each primitive becomes its own mesh
      for (size_t p = 0; p < srcMesh->primitives_count; p++) {
        cgltf_primitive *primitive = &srcMesh->primitives[p];
        Mesh *dstMesh = &model.meshes[meshIndex];
        dstMesh->textureId = -1;
        dstMesh->materialColor = 0xFFFFFFFF;
        dstMesh->alphaMode = 0;       // OPAQUE
        dstMesh->alphaCutoff = 0.5f;  // glTF default
        dstMesh->doubleSided = 0;
        dstMesh->wrapU = 10497;       // REPEAT
        dstMesh->wrapV = 10497;       // REPEAT

        // Get material data for this primitive
        if (primitive->material) {
          cgltf_material *mat = primitive->material;

          // Alpha mode: cgltf_alpha_mode_opaque=0, mask=1, blend=2
          dstMesh->alphaMode = (int)mat->alpha_mode;
          dstMesh->alphaCutoff = mat->alpha_cutoff;
          dstMesh->doubleSided = mat->double_sided ? 1 : 0;

          if (mat->has_pbr_metallic_roughness) {
            cgltf_pbr_metallic_roughness *pbr =
                &mat->pbr_metallic_roughness;

            float *bc = pbr->base_color_factor;
            uint8_t a = (uint8_t)(bc[3] * 255.0f);
            uint8_t r = (uint8_t)(bc[0] * 255.0f);
            uint8_t g = (uint8_t)(bc[1] * 255.0f);
            uint8_t b = (uint8_t)(bc[2] * 255.0f);
            dstMesh->materialColor = (a << 24) | (r << 16) | (g << 8) | b;

            /* Auto-promote to BLEND if baseColorFactor alpha < 1 but
               glTF says OPAQUE (common in ripped/converted content). */
            if (bc[3] < 0.999f && dstMesh->alphaMode == 0) {
              printf("Mesh %d: baseColorFactor alpha=%.2f, promoting OPAQUE -> TRANSPARENT\n",
                     meshIndex, bc[3]);
              dstMesh->alphaMode = 2;
            }

            printf("Mesh %d (primitive %zu) material color: 0x%08X\n", meshIndex,
                   p, dstMesh->materialColor);

            if (pbr->base_color_texture.texture) {
              dstMesh->textureId =
                  pbr->base_color_texture.texture->image - data->images;
              printf("Mesh %d using texture ID: %d\n", meshIndex,
                     dstMesh->textureId);

              // UV wrap mode from sampler
              cgltf_sampler *sampler = pbr->base_color_texture.texture->sampler;
              if (sampler) {
                dstMesh->wrapU = sampler->wrap_s;
                dstMesh->wrapV = sampler->wrap_t;
              }
            }
            if (pbr->base_color_texture.texture &&
                pbr->base_color_texture.texture->image) {
              cgltf_image *img = pbr->base_color_texture.texture->image;
              if (img->uri) {
                textureNames[dstMesh->textureId] = img->uri;
              } else if (img->name) {
                textureNames[dstMesh->textureId] = img->name;
              }

              /* Auto-promote OPAQUE to CUTOUT if the texture actually
                 has an alpha channel (common in game rips / bad exports). */
              if (dstMesh->alphaMode == 0) {
                char srcDir[256] = ".";
                strncpy(srcDir, filename, sizeof(srcDir) - 1);
                char *sl = strrchr(srcDir, '/');
                if (!sl) sl = strrchr(srcDir, '\\');
                if (sl) *sl = '\0'; else strcpy(srcDir, ".");

                if (CgltfImageHasAlpha(img, srcDir)) {
                  printf("Mesh %d: texture has alpha channel, promoting OPAQUE -> CUTOUT\n",
                         meshIndex);
                  dstMesh->alphaMode = 1;
                }
              }
            }
          }

          const char *alphaNames[] = {"OPAQUE", "CUTOUT", "TRANSPARENT"};
          printf("Mesh %d material: alpha=%s cutoff=%.2f doubleSided=%d\n",
                 meshIndex, alphaNames[dstMesh->alphaMode],
                 dstMesh->alphaCutoff, dstMesh->doubleSided);
        }

        // Count vertices and indices for this primitive
        int totalVertices = 0;
        int totalIndices = 0;

        for (size_t a = 0; a < primitive->attributes_count; a++) {
          if (primitive->attributes[a].type == cgltf_attribute_type_position) {
            totalVertices = (int)primitive->attributes[a].data->count;
            break;
          }
        }
        if (primitive->indices) {
          totalIndices = (int)primitive->indices->count;
        }

        // Allocate buffers
        dstMesh->vertices = (Vertex *)calloc(totalVertices, sizeof(Vertex));
        dstMesh->originalVertices =
            (Vertex *)calloc(totalVertices, sizeof(Vertex));
        dstMesh->animatedVertices =
            (Vertex *)calloc(totalVertices, sizeof(Vertex));
        dstMesh->indices =
            (unsigned int *)calloc(totalIndices, sizeof(unsigned int));
        dstMesh->stripLengths = NULL;
        dstMesh->stripCount = 0;
        dstMesh->looseIndexCount =
            totalIndices; // Default to all loose if loading from GLTF w/o
                          // stripping

        for (int v = 0; v < totalVertices; v++) {
          dstMesh->vertices[v].b = 255;
          dstMesh->vertices[v].g = 255;
          dstMesh->vertices[v].r = 255;
          dstMesh->vertices[v].a = 255;
        }

        // Load vertex attributes
        for (size_t a = 0; a < primitive->attributes_count; a++) {
          cgltf_attribute *attr = &primitive->attributes[a];
          cgltf_accessor *accessor = attr->data;

          switch (attr->type) {
          case cgltf_attribute_type_position: {
            for (size_t v = 0; v < accessor->count; v++) {
              cgltf_accessor_read_float(accessor, v, &dstMesh->vertices[v].x,
                                        3);
              dstMesh->originalVertices[v].x = dstMesh->vertices[v].x;
              dstMesh->originalVertices[v].y = dstMesh->vertices[v].y;
              dstMesh->originalVertices[v].z = dstMesh->vertices[v].z;
            }
          } break;

          case cgltf_attribute_type_normal: {
            for (size_t v = 0; v < accessor->count; v++) {
              float temp[3];
              cgltf_accessor_read_float(accessor, v, temp, 3);
              dstMesh->vertices[v].nx = (int8_t)(temp[0] * 127.0f);
              dstMesh->vertices[v].ny = (int8_t)(temp[1] * 127.0f);
              dstMesh->vertices[v].nz = (int8_t)(temp[2] * 127.0f);
              dstMesh->originalVertices[v].nx = dstMesh->vertices[v].nx;
              dstMesh->originalVertices[v].ny = dstMesh->vertices[v].ny;
              dstMesh->originalVertices[v].nz = dstMesh->vertices[v].nz;
            }
          } break;

          case cgltf_attribute_type_texcoord: {
            if (attr->index == 0) { // Only TEXCOORD_0
              for (size_t v = 0; v < accessor->count; v++) {
                cgltf_accessor_read_float(accessor, v, &dstMesh->vertices[v].u,
                                          2);
                dstMesh->originalVertices[v].u = dstMesh->vertices[v].u;
                dstMesh->originalVertices[v].v = dstMesh->vertices[v].v;
              }
            }
          } break;

          case cgltf_attribute_type_joints: {
            for (size_t v = 0; v < accessor->count; v++) {
              cgltf_uint jointIds[4] = {0};
              cgltf_accessor_read_uint(accessor, v, jointIds, 4);
              if (jointIds[0] > 255) {
                printf("Warning: Joint ID %u exceeds uint8_t range\n",
                       jointIds[0]);
                jointIds[0] = 255;
              }
              dstMesh->vertices[v].boneId = (uint8_t)jointIds[0];
              dstMesh->originalVertices[v].boneId = (uint8_t)jointIds[0];
            }
          } break;

          case cgltf_attribute_type_weights: {
            for (size_t v = 0; v < accessor->count; v++) {
              float weights[4] = {0};
              cgltf_accessor_read_float(accessor, v, weights, 4);
              dstMesh->vertices[v].boneWeight = weights[0];
              dstMesh->originalVertices[v].boneWeight = weights[0];
            }
          } break;

          case cgltf_attribute_type_color: {
            for (size_t v = 0; v < accessor->count; v++) {
              float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
              cgltf_accessor_read_float(accessor, v, color, 4);
              // Store as BGRA
              dstMesh->vertices[v].b = (uint8_t)(color[2] * 255.0f);
              dstMesh->vertices[v].g = (uint8_t)(color[1] * 255.0f);
              dstMesh->vertices[v].r = (uint8_t)(color[0] * 255.0f);
              dstMesh->vertices[v].a = 255;
              dstMesh->originalVertices[v].b = dstMesh->vertices[v].b;
              dstMesh->originalVertices[v].g = dstMesh->vertices[v].g;
              dstMesh->originalVertices[v].r = dstMesh->vertices[v].r;
              dstMesh->originalVertices[v].a = dstMesh->vertices[v].a;
            }
          } break;

          default:
            break;
          }
        }

        if (primitive->indices) {
          for (size_t i = 0; i < primitive->indices->count; i++) {
            dstMesh->indices[i] =
                (unsigned int)cgltf_accessor_read_index(primitive->indices, i);
          }
        }

        dstMesh->vertexCount = totalVertices;
        dstMesh->indexCount = totalIndices;

        printf("Loaded mesh %d with %d vertices and %d indices\n", meshIndex,
               dstMesh->vertexCount, dstMesh->indexCount);

        meshIndex++;
      }
    }
  }

  ExtractAndConvertTextures(data, filename);

  cgltf_free(data);
  return true;
}

void WeldVertices(Mesh *mesh, float threshold = 0.001f) {
  printf("  Welding vertices for mesh...\n");
  printf("  Initial vertex count: %d\n", mesh->vertexCount);

  std::map<std::string, int> vertexMap;
  std::vector<Vertex> weldedVerts;
  std::vector<int> remapTable(mesh->vertexCount);

  // Build map with position AND UV as key
  for (int i = 0; i < mesh->vertexCount; i++) {
    char key[128];
    snprintf(key, sizeof(key), "%.3f,%.3f,%.3f|%.4f,%.4f", mesh->vertices[i].x,
             mesh->vertices[i].y, mesh->vertices[i].z, mesh->vertices[i].u,
             mesh->vertices[i].v);

    auto it = vertexMap.find(key);
    if (it == vertexMap.end()) {
      remapTable[i] = weldedVerts.size();
      vertexMap[key] = weldedVerts.size();
      weldedVerts.push_back(mesh->vertices[i]);
    } else {
      remapTable[i] = it->second;
      // Don't average anything - use exact match only
    }
  }

  // Remap indices
  for (int i = 0; i < mesh->indexCount; i++) {
    mesh->indices[i] = remapTable[mesh->indices[i]];
  }

  // Replace vertex buffers
  free(mesh->vertices);
  free(mesh->originalVertices);
  free(mesh->animatedVertices);

  mesh->vertexCount = weldedVerts.size();
  mesh->vertices = (Vertex *)calloc(mesh->vertexCount, sizeof(Vertex));
  mesh->originalVertices = (Vertex *)calloc(mesh->vertexCount, sizeof(Vertex));
  mesh->animatedVertices = (Vertex *)calloc(mesh->vertexCount, sizeof(Vertex));

  memcpy(mesh->vertices, weldedVerts.data(),
         sizeof(Vertex) * mesh->vertexCount);
  memcpy(mesh->originalVertices, weldedVerts.data(),
         sizeof(Vertex) * mesh->vertexCount);

  printf("  Welded vertex count: %d (removed %d duplicates)\n",
         mesh->vertexCount, (int)remapTable.size() - mesh->vertexCount);
}

void SortVerticesByBoneId(MeshTriStrips &tristrips) {
  if (tristrips.vertices.empty())
    return;

  // Build sorted order by bone ID
  std::vector<uint32_t> sortedOrder(tristrips.vertices.size());
  for (uint32_t i = 0; i < sortedOrder.size(); i++) {
    sortedOrder[i] = i;
  }

  std::stable_sort(
      sortedOrder.begin(), sortedOrder.end(), [&](uint32_t a, uint32_t b) {
        return tristrips.vertices[a].boneId < tristrips.vertices[b].boneId;
      });

  // Build reverse mapping: old index -> new index
  std::vector<uint32_t> remapTable(tristrips.vertices.size());
  for (uint32_t newIdx = 0; newIdx < sortedOrder.size(); newIdx++) {
    remapTable[sortedOrder[newIdx]] = newIdx;
  }

  // Reorder vertices
  std::vector<Vertex> sortedVerts(tristrips.vertices.size());
  for (uint32_t i = 0; i < sortedOrder.size(); i++) {
    sortedVerts[i] = tristrips.vertices[sortedOrder[i]];
  }
  tristrips.vertices = std::move(sortedVerts);

  // Remap strip indices
  for (auto &strip : tristrips.strips) {
    for (auto &idx : strip.indices) {
      idx = remapTable[idx];
    }
  }

  // Remap loose triangle indices
  for (auto &idx : tristrips.looseTriangles) {
    idx = remapTable[idx];
  }
}

void CreateTristrippedModel(const Model *sourceModel, Model *destModel) {
  printf("Creating tristripped model...\n");

  // Copy skeleton pointer and allocate new mesh array
  destModel->skeleton = sourceModel->skeleton;
  destModel->meshCount = sourceModel->meshCount;
  destModel->meshes = (Mesh *)calloc(destModel->meshCount, sizeof(Mesh));

  // Process each mesh from the source model
  for (int m = 0; m < sourceModel->meshCount; m++) {
    const Mesh *srcMesh = &sourceModel->meshes[m];
    Mesh *dstMesh = &destModel->meshes[m];
    dstMesh->textureId = srcMesh->textureId;
    dstMesh->materialColor = srcMesh->materialColor;
    dstMesh->alphaMode = srcMesh->alphaMode;
    dstMesh->alphaCutoff = srcMesh->alphaCutoff;
    dstMesh->doubleSided = srcMesh->doubleSided;
    dstMesh->wrapU = srcMesh->wrapU;
    dstMesh->wrapV = srcMesh->wrapV;

    printf("Processing mesh %d of %d...\n", m + 1, sourceModel->meshCount);
    printf("Source mesh has %d vertices and %d indices\n", srcMesh->vertexCount,
           srcMesh->indexCount);

    // Use the new ExtractTriStrips function to get optimized data
    MeshTriStrips tristrips = ExtractTriStrips(srcMesh);

    // SortVerticesByBoneId(tristrips);

    // Allocate destination mesh memory
    dstMesh->vertexCount = tristrips.vertices.size();
    dstMesh->vertices = (Vertex *)calloc(dstMesh->vertexCount, sizeof(Vertex));
    dstMesh->animatedVertices =
        (Vertex *)calloc(dstMesh->vertexCount, sizeof(Vertex));

    // Copy optimized vertices to bind pose buffer (already done)
    memcpy(dstMesh->vertices, tristrips.vertices.data(),
           dstMesh->vertexCount * sizeof(Vertex));

    // Calculate total indices needed
    size_t totalIndices = 0;
    for (const auto &strip : tristrips.strips) {
      totalIndices += strip.indices.size();
    }
    totalIndices += tristrips.looseTriangles.size();

    // Allocate and fill index buffer
    dstMesh->indexCount = totalIndices;
    dstMesh->indices =
        (unsigned int *)calloc(dstMesh->indexCount, sizeof(unsigned int));

    dstMesh->stripCount = tristrips.strips.size();
    dstMesh->stripLengths =
        (unsigned int *)calloc(dstMesh->stripCount, sizeof(unsigned int));
    dstMesh->looseIndexCount = tristrips.looseTriangles.size();

    // Copy indices - first the strips
    size_t indexOffset = 0;
    for (size_t s = 0; s < tristrips.strips.size(); s++) {
      const auto &strip = tristrips.strips[s];
      dstMesh->stripLengths[s] = strip.indices.size();

      for (size_t i = 0; i < strip.indices.size(); i++) {
        // Store RAW index
        dstMesh->indices[indexOffset++] = strip.indices[i];
      }
    }

    //   loose triangles
    for (uint32_t idx : tristrips.looseTriangles) {
      dstMesh->indices[indexOffset++] = idx;
    }

    printf("Mesh %d processed:\n", m + 1);
    printf("  Original vertices:    %d\n", srcMesh->vertexCount);
    printf("  Optimized vertices:   %d\n", dstMesh->vertexCount);
    printf("  Total strips:         %zu\n", tristrips.strips.size());
    printf("  Loose triangles:      %zu\n",
           tristrips.looseTriangles.size() / 3);
    printf("  Total indices:        %d\n", dstMesh->indexCount);
  }

  printf("Tristripped model creation complete!\n");
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Usage: %s [-bake] <model_file>\n", argv[0]);
    printf("  -bake    Bake vertex lighting (AO, shadows, etc.)\n");
    printf("Supported formats: .gltf, .glb\n");
    return 1;
  }

  // Parse arguments
  bool bakeLighting = false;
  const char *inputFilename = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-bake") == 0) {
      bakeLighting = true;
    } else if (argv[i][0] != '-') {
      inputFilename = argv[i];
    }
  }

  if (!inputFilename) {
    printf("Error: No input file specified\n");
    printf("Usage: %s [-bake] <model_file>\n", argv[0]);
    return 1;
  }

  const char *extension = strrchr(inputFilename, '.');
  if (!extension) {
    printf("Error: Could not determine file extension\n");
    return 1;
  }

  std::string ext(extension);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  if (ext != ".gltf" && ext != ".glb") {
    printf("Unsupported file format: %s\n", extension);
    printf("Supported formats: .gltf, .glb\n");
    return 1;
  }

  printf("Loading GLTF/GLB: %s\n", inputFilename);
  if (bakeLighting) {
    printf("Lighting bake: ENABLED\n");
  } else {
    printf("Lighting bake: DISABLED (use -bake to enable)\n");
  }

  if (!LoadGLTF(inputFilename)) {
    printf("Failed to load file: %s\n", inputFilename);
    return 1;
  }

  printf("\n=== Vertex Welding Phase ===\n");
  for (int i = 0; i < model.meshCount; i++) {
    WeldVertices(&model.meshes[i]);
  }
  printf("=== Vertex Welding Complete ===\n\n");

  CreateTristrippedModel(&model, &tristrippedModel);

  char outputFilename[256] = {0};
  size_t baseNameLength = extension - inputFilename;
  strncpy(outputFilename, inputFilename, baseNameLength);
  strcat(outputFilename, ".dms");

  if (tristrippedModel.meshCount > 0) {
    printf("Exporting to: %s\n", outputFilename);
    ExportTristrippedModel(&tristrippedModel, outputFilename, bakeLighting);
    printf("Export complete!\n");

    if (!textureNames.empty()) {
      printf("\nTexture ID Mapping:\n");
      for (const auto &pair : textureNames) {
        printf("  Texture ID %d = %s\n", pair.first, pair.second.c_str());
      }
    }
  } else {
    printf("Error: No meshes to export\n");
  }

  Cleanup();
  return 0;
}

void Cleanup(void) {
  if (skeleton.bones) {
    free(skeleton.bones);
  }

  if (skeleton.animations) {
    for (int i = 0; i < skeleton.animCount; i++) {
      if (skeleton.animations[i].framePoses) {
        free(skeleton.animations[i].framePoses);
      }
    }
    free(skeleton.animations);
  }

  if (model.meshes) {
    for (int i = 0; i < model.meshCount; i++) {
      if (model.meshes[i].vertices)
        free(model.meshes[i].vertices);
      if (model.meshes[i].originalVertices)
        free(model.meshes[i].originalVertices);
      if (model.meshes[i].animatedVertices)
        free(model.meshes[i].animatedVertices);
      if (model.meshes[i].indices)
        free(model.meshes[i].indices);
      if (model.meshes[i].stripLengths)
        free(model.meshes[i].stripLengths);
    }
    free(model.meshes);
  }

  if (tristrippedModel.meshes) {
    for (int i = 0; i < tristrippedModel.meshCount; i++) {
      if (tristrippedModel.meshes[i].vertices)
        free(tristrippedModel.meshes[i].vertices);
      if (tristrippedModel.meshes[i].animatedVertices)
        free(tristrippedModel.meshes[i].animatedVertices);
      if (tristrippedModel.meshes[i].indices)
        free(tristrippedModel.meshes[i].indices);
      if (tristrippedModel.meshes[i].stripLengths)
        free(tristrippedModel.meshes[i].stripLengths);
    }
    free(tristrippedModel.meshes);
  }
}

bool can_join_strips(const std::vector<size_t> &strip1,
                     const std::vector<size_t> &strip2) {
  if (strip1.size() < 3 || strip2.size() < 3)
    return false;

  // Only join when edges match in same order (winding compatible)
  return (strip1[strip1.size() - 1] == strip2[1] &&
          strip1[strip1.size() - 2] == strip2[0]);
}

std::vector<std::vector<size_t>>
join_strips(const triangle_stripper::primitive_vector &originalStrips) {
  std::vector<std::vector<size_t>> result;
  std::vector<bool> used(originalStrips.size(), false);
  std::vector<std::vector<size_t>> strips;

  for (const auto &prim : originalStrips) {
    if (prim.Type == triangle_stripper::TRIANGLE_STRIP) {
      strips.push_back(
          std::vector<size_t>(prim.Indices.begin(), prim.Indices.end()));
    }
  }

  for (size_t i = 0; i < strips.size(); i++) {
    if (used[i])
      continue;

    std::vector<size_t> current_strip = strips[i];
    used[i] = true;

    bool found_join;
    do {
      found_join = false;
      for (size_t j = 0; j < strips.size(); j++) {
        if (!used[j] && can_join_strips(current_strip, strips[j])) {
          current_strip.insert(current_strip.end(), strips[j].begin() + 2,
                               strips[j].end());
          used[j] = true;
          found_join = true;
          break;
        }
      }
    } while (found_join);

    result.push_back(current_strip);
  }
  return result;
}

void optimize_mesh() {
  if (triangles.empty()) {
    printf("Warning: No triangles to optimize. Optimization did not happen.\n");
    return;
  }

  using namespace triangle_stripper;

  printf("=== Strip Optimization ===\n");
  printf("Input triangles: %zu\n", triangles.size());

  // Build indices from triangle vertexIds directly
  indices Indices;
  for (const auto &tri : triangles) {
    for (int i = 0; i < 3; i++) {
      Indices.push_back(tri.vertices[i].vertexId);
    }
  }

  // Generate strips
  primitive_vector primitives;
  tri_stripper stripper(Indices);
  stripper.SetMinStripSize(0);
  stripper.SetCacheSize(0);
  stripper.SetBackwardSearch(true);
  stripper.SetPushCacheHits(true);
  stripper.Strip(&primitives);

  // Join compatible strips
  auto joined_strips = join_strips(primitives);

  // Count stats
  size_t strip_triangles = 0;
  size_t list_triangles = 0;

  for (const auto &prim : primitives) {
    if (prim.Type == TRIANGLES) {
      list_triangles += prim.Indices.size() / 3;
    }
  }

  for (const auto &strip : joined_strips) {
    if (strip.size() >= 3) {
      strip_triangles += strip.size() - 2;
    }
  }

  float strip_ratio =
      (strip_triangles + list_triangles) > 0
          ? (float)strip_triangles / (float)(strip_triangles + list_triangles)
          : 0;

  size_t total_strip_vertices = 0;
  size_t max_strip_length = 0;
  for (const auto &strip : joined_strips) {
    total_strip_vertices += strip.size();
    max_strip_length = std::max(max_strip_length, strip.size());
  }

  float avg_strip_length =
      joined_strips.size() > 0
          ? (float)total_strip_vertices / (float)joined_strips.size()
          : 0;
  float vertex_reuse =
      joined_strips.size() > 0
          ? (float)strip_triangles * 3.0f / (float)total_strip_vertices
          : 0;

  printf("Optimization complete:\n");
  printf("- Strips: %zu\n", joined_strips.size());
  printf("- Strip triangles: %zu\n", strip_triangles);
  printf("- List triangles: %zu\n", list_triangles);
  printf("- Total triangles: %zu\n", strip_triangles + list_triangles);

  printf("\nEfficiency Metrics:\n");
  printf(" - Strip ratio: %.2f%%\n", strip_ratio * 100.0f);
  printf(" - Average strip length: %.2f vertices\n", avg_strip_length);
  printf(" - Longest strip: %zu vertices\n", max_strip_length);
  printf(" - Vertex reuse factor: %.2f\n", vertex_reuse);
  printf("=== End of Strip Optimization Statistics ===\n");

  // Store raw strip indices - these ARE the vertexIds now
  g_raw_strips.clear();
  for (const auto &strip : joined_strips) {
    std::vector<uint32_t> raw;
    for (size_t idx : strip) {
      raw.push_back(idx);
    }
    g_raw_strips.push_back(raw);
  }

  g_loose_triangles.clear();
  for (const auto &prim : primitives) {
    if (prim.Type == TRIANGLES) {
      for (size_t idx : prim.Indices) {
        g_loose_triangles.push_back(idx);
      }
    }
  }

  // Mark triangles that are in strips vs loose
  for (auto &tri : triangles) {
    tri.materialId = 0; // Will be updated if in a strip
  }

  // Mark strip triangles with their strip ID
  for (size_t strip_idx = 0; strip_idx < joined_strips.size(); strip_idx++) {
    const auto &strip = joined_strips[strip_idx];
  }
}

struct LitColor {
  uint8_t r, g, b;
};

/* ================================================================
 * BVH for fast ray tracing during lightmap bake
 * ================================================================ */

struct AABB {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

struct BVHNode {
    AABB box;
    int left;      /* child index, or -1 for leaf */
    int right;
    int triStart;  /* leaf: first triangle index */
    int triCount;  /* leaf: number of triangles */
};

static std::vector<BVHNode> g_bvh;
static std::vector<int>     g_bvhTriIdx;
static bool                 g_bvhBuilt = false;

static AABB TriAABB(const std::array<float,9>& t) {
    return {
        fminf(fminf(t[0],t[3]),t[6]), fminf(fminf(t[1],t[4]),t[7]), fminf(fminf(t[2],t[5]),t[8]),
        fmaxf(fmaxf(t[0],t[3]),t[6]), fmaxf(fmaxf(t[1],t[4]),t[7]), fmaxf(fmaxf(t[2],t[5]),t[8])
    };
}

static AABB MergeAABB(const AABB& a, const AABB& b) {
    return {
        fminf(a.minX,b.minX), fminf(a.minY,b.minY), fminf(a.minZ,b.minZ),
        fmaxf(a.maxX,b.maxX), fmaxf(a.maxY,b.maxY), fmaxf(a.maxZ,b.maxZ)
    };
}

static bool RayAABB(const AABB& box, float ox, float oy, float oz,
                    float idx, float idy, float idz, float maxDist) {
    float t1 = (box.minX - ox) * idx, t2 = (box.maxX - ox) * idx;
    float t3 = (box.minY - oy) * idy, t4 = (box.maxY - oy) * idy;
    float t5 = (box.minZ - oz) * idz, t6 = (box.maxZ - oz) * idz;
    float tmin = fmaxf(fmaxf(fminf(t1,t2), fminf(t3,t4)), fminf(t5,t6));
    float tmax = fminf(fminf(fmaxf(t1,t2), fmaxf(t3,t4)), fmaxf(t5,t6));
    return tmax >= fmaxf(tmin, 0.0f) && tmin < maxDist;
}

static int BuildBVHRec(int* idxs, int count) {
    int nodeIdx = (int)g_bvh.size();
    g_bvh.push_back({});

    /* Compute bounds */
    AABB bounds = TriAABB(g_rtTriangles[idxs[0]]);
    for (int i = 1; i < count; i++)
        bounds = MergeAABB(bounds, TriAABB(g_rtTriangles[idxs[i]]));
    g_bvh[nodeIdx].box = bounds;

    if (count <= 4) {
        /* Leaf */
        g_bvh[nodeIdx].triStart = (int)g_bvhTriIdx.size();
        g_bvh[nodeIdx].triCount = count;
        g_bvh[nodeIdx].left = g_bvh[nodeIdx].right = -1;
        for (int i = 0; i < count; i++)
            g_bvhTriIdx.push_back(idxs[i]);
        return nodeIdx;
    }

    /* Pick longest axis, split at midpoint */
    float dx = bounds.maxX - bounds.minX;
    float dy = bounds.maxY - bounds.minY;
    float dz = bounds.maxZ - bounds.minZ;
    int axis = (dx >= dy && dx >= dz) ? 0 : (dy >= dz) ? 1 : 2;

    float mid = 0;
    for (int i = 0; i < count; i++) {
        auto& t = g_rtTriangles[idxs[i]];
        mid += (t[axis] + t[axis+3] + t[axis+6]);
    }
    mid /= (count * 3.0f);

    /* Partition */
    int split = 0;
    for (int i = 0; i < count; i++) {
        auto& t = g_rtTriangles[idxs[i]];
        float c = (t[axis] + t[axis+3] + t[axis+6]) / 3.0f;
        if (c < mid) {
            std::swap(idxs[i], idxs[split]);
            split++;
        }
    }
    if (split == 0 || split == count)
        split = count / 2;

    g_bvh[nodeIdx].triCount = 0;
    int leftIdx = BuildBVHRec(idxs, split);
    int rightIdx = BuildBVHRec(idxs + split, count - split);
    g_bvh[nodeIdx].left = leftIdx;
    g_bvh[nodeIdx].right = rightIdx;
    return nodeIdx;
}

static void BuildBVH() {
    int n = (int)g_rtTriangles.size();
    if (n == 0) { g_bvhBuilt = false; return; }

    g_bvh.clear();
    g_bvhTriIdx.clear();
    g_bvh.reserve(n * 2);
    g_bvhTriIdx.reserve(n);

    std::vector<int> idxs(n);
    for (int i = 0; i < n; i++) idxs[i] = i;
    BuildBVHRec(idxs.data(), n);
    g_bvhBuilt = true;
    printf("BVH built: %zu nodes, %zu tri refs\n", g_bvh.size(), g_bvhTriIdx.size());
}

void BuildRTScene(const Model *model) {
  g_rtTriangles.clear();
  for (int m = 0; m < model->meshCount; m++) {
    const Mesh *mesh = &model->meshes[m];
    if (!mesh->indices || mesh->indexCount < 3)
      continue;

    int i = 0;
    while (i < mesh->indexCount) {
      uint32_t rawIndex = mesh->indices[i];
      bool isStrip = (rawIndex & 0x80000000) != 0;

      if (isStrip) {
        uint32_t sId = (rawIndex >> 24) & 0x7F;
        int stripStart = i;

        while (i < mesh->indexCount && (mesh->indices[i] & 0x80000000) &&
               ((mesh->indices[i] >> 24) & 0x7F) == sId) {
          i++;
        }
        int stripLen = i - stripStart;

        for (int j = 0; j < stripLen - 2; j++) {
          uint32_t i0 = mesh->indices[stripStart + j] & 0x00FFFFFF;
          uint32_t i1 = mesh->indices[stripStart + j + 1] & 0x00FFFFFF;
          uint32_t i2 = mesh->indices[stripStart + j + 2] & 0x00FFFFFF;

          if (j & 1)
            std::swap(i0, i1);

          if (i0 < (uint32_t)mesh->vertexCount &&
              i1 < (uint32_t)mesh->vertexCount &&
              i2 < (uint32_t)mesh->vertexCount) {
            const Vertex &v0 = mesh->vertices[i0];
            const Vertex &v1 = mesh->vertices[i1];
            const Vertex &v2 = mesh->vertices[i2];
            g_rtTriangles.push_back(
                {v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z});
          }
        }
      } else {
        if (i + 2 < mesh->indexCount) {
          uint32_t i0 = mesh->indices[i] & 0x00FFFFFF;
          uint32_t i1 = mesh->indices[i + 1] & 0x00FFFFFF;
          uint32_t i2 = mesh->indices[i + 2] & 0x00FFFFFF;

          if (i0 < (uint32_t)mesh->vertexCount &&
              i1 < (uint32_t)mesh->vertexCount &&
              i2 < (uint32_t)mesh->vertexCount) {
            const Vertex &v0 = mesh->vertices[i0];
            const Vertex &v1 = mesh->vertices[i1];
            const Vertex &v2 = mesh->vertices[i2];
            g_rtTriangles.push_back(
                {v0.x, v0.y, v0.z, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z});
          }
        }
        i += 3;
      }
    }
  }
  printf("Built RT scene: %zu triangles\n", g_rtTriangles.size());
  BuildBVH();
}

static inline bool RayTriHit(const std::array<float,9>& tri,
                              float ox, float oy, float oz,
                              float dx, float dy, float dz, float maxDist) {
    float e1x = tri[3]-tri[0], e1y = tri[4]-tri[1], e1z = tri[5]-tri[2];
    float e2x = tri[6]-tri[0], e2y = tri[7]-tri[1], e2z = tri[8]-tri[2];
    float hx = dy*e2z - dz*e2y, hy = dz*e2x - dx*e2z, hz = dx*e2y - dy*e2x;
    float a = e1x*hx + e1y*hy + e1z*hz;
    if (fabsf(a) < 1e-7f) return false;
    float f = 1.0f / a;
    float sx = ox-tri[0], sy = oy-tri[1], sz = oz-tri[2];
    float u = f * (sx*hx + sy*hy + sz*hz);
    if (u < 0.0f || u > 1.0f) return false;
    float qx = sy*e1z - sz*e1y, qy = sz*e1x - sx*e1z, qz = sx*e1y - sy*e1x;
    float v = f * (dx*qx + dy*qy + dz*qz);
    if (v < 0.0f || u+v > 1.0f) return false;
    float t = f * (e2x*qx + e2y*qy + e2z*qz);
    return (t > 1e-4f && t < maxDist);
}

bool RayHit(float ox, float oy, float oz, float dx, float dy, float dz,
            float maxDist) {
  if (!g_bvhBuilt) return false;

  float idx = 1.0f / (fabsf(dx) > 1e-8f ? dx : (dx >= 0 ? 1e-8f : -1e-8f));
  float idy = 1.0f / (fabsf(dy) > 1e-8f ? dy : (dy >= 0 ? 1e-8f : -1e-8f));
  float idz = 1.0f / (fabsf(dz) > 1e-8f ? dz : (dz >= 0 ? 1e-8f : -1e-8f));

  int stack[64];
  int sp = 0;
  stack[sp++] = 0;  /* root */

  while (sp > 0) {
    const BVHNode& node = g_bvh[stack[--sp]];
    if (!RayAABB(node.box, ox, oy, oz, idx, idy, idz, maxDist))
      continue;
    if (node.left == -1) {
      /* Leaf */
      for (int i = 0; i < node.triCount; i++) {
        if (RayTriHit(g_rtTriangles[g_bvhTriIdx[node.triStart + i]],
                      ox, oy, oz, dx, dy, dz, maxDist))
          return true;
      }
    } else {
      stack[sp++] = node.left;
      stack[sp++] = node.right;
    }
  }
  return false;
}

float ComputeAO(float px, float py, float pz, float nx, float ny, float nz,
                int samples) {
  thread_local std::mt19937 rng(12345 + omp_get_thread_num());
  thread_local std::uniform_real_distribution<float> dist(0.0f, 1.0f);

  float ao = 0.0f;
  float bias = 0.001f;
  float maxDist = 2.0f; // AO radius

  for (int i = 0; i < samples; i++) {
    float u1 = dist(rng), u2 = dist(rng);
    float r = sqrtf(u1), theta = 6.283185f * u2;
    float lx = r * cosf(theta), ly = r * sinf(theta), lz = sqrtf(1.0f - u1);

    // Build tangent frame
    float upX = (fabsf(nx) < 0.9f) ? 1.0f : 0.0f,
          upY = (fabsf(nx) < 0.9f) ? 0.0f : 1.0f;
    float tx = upY * nz, ty = -upX * nz, tz = upX * ny - upY * nx;
    float tlen = sqrtf(tx * tx + ty * ty + tz * tz);
    tx /= tlen;
    ty /= tlen;
    tz /= tlen;
    float bx = ny * tz - nz * ty, by = nz * tx - nx * tz,
          bz = nx * ty - ny * tx;

    float dx = lx * tx + ly * bx + lz * nx;
    float dy = lx * ty + ly * by + lz * ny;
    float dz = lx * tz + ly * bz + lz * nz;

    if (!RayHit(px + nx * bias, py + ny * bias, pz + nz * bias, dx, dy, dz,
                maxDist))
      ao += 1.0f;
  }
  return ao / samples;
}

LitColor CalculateVertexLighting(float px, float py, float pz, float nx,
                                 float ny, float nz, uint8_t baseR,
                                 uint8_t baseG, uint8_t baseB, uint8_t vertR,
                                 uint8_t vertG, uint8_t vertB) {
  float ao = ComputeAO(px, py, pz, nx, ny, nz, 64);

  // Key light
  float keyX = 0.4f, keyY = 0.8f, keyZ = 0.4f;
  float klen = sqrtf(keyX * keyX + keyY * keyY + keyZ * keyZ);
  keyX /= klen;
  keyY /= klen;
  keyZ /= klen;
  float keyNdotL = fmaxf(0.0f, nx * keyX + ny * keyY + nz * keyZ);
  float keyWrap = (keyNdotL + 0.5f) / 1.5f;
  keyWrap *= keyWrap;

  // Key light shadow
  float shadow = RayHit(px + nx * 0.001f, py + ny * 0.001f, pz + nz * 0.001f,
                        keyX, keyY, keyZ, 100.0f)
                     ? 0.3f
                     : 1.0f;

  // Fill light
  float fillX = -0.5f, fillY = 0.3f, fillZ = -0.3f;
  float flen = sqrtf(fillX * fillX + fillY * fillY + fillZ * fillZ);
  fillX /= flen;
  fillY /= flen;
  fillZ /= flen;
  float fillDiffuse = (nx * fillX + ny * fillY + nz * fillZ) * 0.5f + 0.5f;
  fillDiffuse *= fillDiffuse;

  // Hemisphere ambient
  float hemi = ny * 0.5f + 0.5f;
  float ambR = 0.3f + 0.2f * hemi;
  float ambG = 0.25f + 0.35f * hemi;
  float ambB = 0.2f + 0.6f * hemi;

  // Rim
  float rimX = -0.3f, rimY = 0.2f, rimZ = -0.9f;
  float rlen = sqrtf(rimX * rimX + rimY * rimY + rimZ * rimZ);
  float rim =
      fmaxf(0.0f, -(nx * rimX / rlen + ny * rimY / rlen + nz * rimZ / rlen));
  rim = powf(rim, 2.0f) * 0.2f;

  // Combine (multiply source vertex color with baked lighting)
  float srcR = vertR / 255.0f;
  float srcG = vertG / 255.0f;
  float srcB = vertB / 255.0f;

  float r =
      srcR * (baseR / 255.0f) *
          (ambR * 0.35f * ao + keyWrap * 0.7f * shadow + fillDiffuse * 0.25f) +
      rim;
  float g =
      srcG * (baseG / 255.0f) *
          (ambG * 0.35f * ao + keyWrap * 0.65f * shadow + fillDiffuse * 0.3f) +
      rim;
  float b =
      srcB * (baseB / 255.0f) *
          (ambB * 0.35f * ao + keyWrap * 0.55f * shadow + fillDiffuse * 0.35f) +
      rim;

  // Saturation boost
  float gray = (r + g + b) / 3.0f;
  r = gray + (r - gray) * 1.15f;
  g = gray + (g - gray) * 1.15f;
  b = gray + (b - gray) * 1.15f;

  return {(uint8_t)(fminf(1.0f, fmaxf(0.0f, r)) * 255.0f),
          (uint8_t)(fminf(1.0f, fmaxf(0.0f, g)) * 255.0f),
          (uint8_t)(fminf(1.0f, fmaxf(0.0f, b)) * 255.0f)};
}

void ExportTristrippedModel(const Model *model, const char *filename,
                            bool bakeLighting) {
  FILE *file = fopen(filename, "wb");
  if (!file) {
    printf("Failed to open file for writing: %s\n", filename);
    return;
  }

  // Header
  uint32_t magic = 0x54534D44;
  uint32_t version = 5; // Version 5: material flags per mesh
  uint32_t meshCount = model->meshCount;
  uint32_t boneCount = model->skeleton ? model->skeleton->boneCount : 0;
  bool isAnimated = (boneCount > 0);

  // Build sort order: OPAQUE first, then CUTOUT, then TRANSPARENT
  std::vector<int> meshOrder(meshCount);
  std::iota(meshOrder.begin(), meshOrder.end(), 0);
  std::stable_sort(meshOrder.begin(), meshOrder.end(), [&](int a, int b) {
    return model->meshes[a].alphaMode < model->meshes[b].alphaMode;
  });

  uint32_t opaque_count = 0, cutout_count = 0, transparent_count = 0;
  for (uint32_t i = 0; i < meshCount; i++) {
    int mode = model->meshes[meshOrder[i]].alphaMode;
    if (mode == 0)      opaque_count++;
    else if (mode == 1) cutout_count++;
    else                transparent_count++;
  }

  printf("Mesh sort: %u opaque, %u cutout, %u transparent\n",
         opaque_count, cutout_count, transparent_count);

  fwrite(&magic, sizeof(uint32_t), 1, file);
  fwrite(&version, sizeof(uint32_t), 1, file);
  fwrite(&meshCount, sizeof(uint32_t), 1, file);
  fwrite(&boneCount, sizeof(uint32_t), 1, file);
  fwrite(&opaque_count, sizeof(uint32_t), 1, file);
  fwrite(&cutout_count, sizeof(uint32_t), 1, file);
  fwrite(&transparent_count, sizeof(uint32_t), 1, file);

  printf("Writing file header at %ld\n", ftell(file));

  // Write skeleton data if it exists
  if (isAnimated && model->skeleton) {
    printf("Writing %d bones at %ld\n", boneCount, ftell(file));

    for (uint32_t i = 0; i < boneCount; i++) {
      const Bone &bone = model->skeleton->bones[i];
      fwrite(bone.name, sizeof(char), 64, file);
      fwrite(&bone.parent, sizeof(int), 1, file);
      WriteDMSTransform(file, bone.bindPose);
      WriteMatrixColumnMajor(file, bone.inverseBindMatrix);
    }

    uint32_t animCount = model->skeleton->animCount;
    fwrite(&animCount, sizeof(uint32_t), 1, file);

    printf("Writing %d animations at %ld\n", animCount, ftell(file));

    for (uint32_t i = 0; i < animCount; i++) {
      const Animation *anim = &model->skeleton->animations[i];

      fwrite(anim->name, sizeof(char), 32, file);
      fwrite(&anim->boneCount, sizeof(int), 1, file);
      fwrite(&anim->frameCount, sizeof(int), 1, file);
      fwrite(&anim->duration, sizeof(float), 1, file);

      printf("Writing animation %d: %s (%d frames) at %ld\n", i, anim->name,
             anim->frameCount, ftell(file));

      size_t totalPoses = anim->frameCount * anim->boneCount;
      for (size_t j = 0; j < totalPoses; j++) {
        Transform *pose = &((Transform *)anim->framePoses)[j];
        WriteDMSTransform(file, *pose);
      }
    }
  } else {
    uint32_t animCount = 0;
    fwrite(&animCount, sizeof(uint32_t), 1, file);
  }

  // Write mesh data
  printf("Writing %d meshes at %ld\n", meshCount, ftell(file));

  if (bakeLighting) {
    BuildRTScene(model);
    printf("Baking lighting (AO/Shadows)...\n");
  }

  for (uint32_t m = 0; m < meshCount; m++) {
    const Mesh *mesh = &model->meshes[meshOrder[m]];

    printf("  Mesh %d (orig %d): %d unique vertices, %d total indices\n", m,
           meshOrder[m], mesh->vertexCount, mesh->indexCount);

    // Bake lighting or apply material color
    uint8_t baseA = (mesh->materialColor >> 24) & 0xFF;
    uint8_t baseR = (mesh->materialColor >> 16) & 0xFF;
    uint8_t baseG = (mesh->materialColor >> 8) & 0xFF;
    uint8_t baseB = (mesh->materialColor) & 0xFF;

    if (bakeLighting) {
      printf("    Computing lighting (%d threads)...", omp_get_max_threads());
      fflush(stdout);
      #pragma omp parallel for schedule(dynamic, 64)
      for (int i = 0; i < mesh->vertexCount; i++) {
        Vertex *v = &mesh->vertices[i];
        float vnx = v->nx / 127.0f;
        float vny = v->ny / 127.0f;
        float vnz = v->nz / 127.0f;
        LitColor lit =
            CalculateVertexLighting(v->x, v->y, v->z, vnx, vny, vnz, baseR, baseG,
                                    baseB, v->r, v->g, v->b);
        v->r = lit.r;
        v->g = lit.g;
        v->b = lit.b;
        v->a = (uint8_t)((v->a * baseA) / 255);
      }
      printf(" done.\n");
    } else {
      for (int i = 0; i < mesh->vertexCount; i++) {
        Vertex *v = &mesh->vertices[i];
        v->r = (uint8_t)((v->r * baseR) / 255);
        v->g = (uint8_t)((v->g * baseG) / 255);
        v->b = (uint8_t)((v->b * baseB) / 255);
        v->a = (uint8_t)((v->a * baseA) / 255);
      }
    }

    // Compute bounding sphere
    float bsCx, bsCy, bsCz, bsRadius;
    ComputeBoundingSphere(mesh, &bsCx, &bsCy, &bsCz, &bsRadius);
    printf("    Bounding sphere: center(%.2f, %.2f, %.2f) radius=%.2f\n",
           bsCx, bsCy, bsCz, bsRadius);

    // Count total vertices (flattened)
    uint32_t totalVerts = mesh->indexCount;

    // Write mesh header
    fwrite(&totalVerts, sizeof(uint32_t), 1, file);
    fwrite(&mesh->textureId, sizeof(int), 1, file);
    fwrite(&mesh->materialColor, sizeof(uint32_t), 1, file);

    // Write bounding sphere
    fwrite(&bsCx, sizeof(float), 1, file);
    fwrite(&bsCy, sizeof(float), 1, file);
    fwrite(&bsCz, sizeof(float), 1, file);
    fwrite(&bsRadius, sizeof(float), 1, file);

    // Pack material flags (v5)
    // bits 0-1:  alpha_mode (0=OPAQUE, 1=CUTOUT, 2=TRANSPARENT)
    // bit  2:    double_sided
    // bits 3-4:  blend_mode (0=src_alpha, 1=additive) — default 0
    // bits 5-6:  wrap_u (0=repeat, 1=clamp, 2=mirror)
    // bits 7-8:  wrap_v (same)
    // bit  9:    tex_filter (0=bilinear, 1=nearest)
    // bits 10-11: lighting_mode (0=BAKED, 1=DYNAMIC, 2=UNLIT)
    uint32_t material_flags = 0;
    material_flags |= (mesh->alphaMode & 0x3);
    material_flags |= (mesh->doubleSided & 0x1) << 2;
    // blend mode: default src-alpha (0)
    int wu = (mesh->wrapU == 33071) ? 1 : (mesh->wrapU == 33648) ? 2 : 0;
    int wv = (mesh->wrapV == 33071) ? 1 : (mesh->wrapV == 33648) ? 2 : 0;
    material_flags |= (wu & 0x3) << 5;
    material_flags |= (wv & 0x3) << 7;
    // tex_filter: default bilinear (0)
    // lighting_mode: baked=0, dynamic=1
    material_flags |= (bakeLighting ? 0 : 1) << 10;

    float alphaCutoff = mesh->alphaCutoff;
    fwrite(&material_flags, sizeof(uint32_t), 1, file);
    fwrite(&alphaCutoff, sizeof(float), 1, file);

    const char *alphaNames[] = {"OPAQUE", "CUTOUT", "TRANSPARENT"};
    printf("    Material: alpha=%s flags=0x%08X cutoff=%.2f\n",
           alphaNames[mesh->alphaMode & 0x3], material_flags, alphaCutoff);

    // Write vertices with baked EOL flags
    printf("    Writing %d direct vertices with baked flags...\n", totalVerts);

    // Build strip end positions for flag lookup
    std::vector<uint32_t> stripEnds;
    uint32_t pos = 0;

    // Explicit strips
    for (int s = 0; s < mesh->stripCount; s++) {
      pos += mesh->stripLengths[s];
      stripEnds.push_back(pos - 1); // Last vertex of this strip
    }

    // Loose triangles become 3-vertex strips
    uint32_t stripIdxCount = 0;
    for (int s = 0; s < mesh->stripCount; s++) {
      stripIdxCount += mesh->stripLengths[s];
    }
    int looseCount = mesh->indexCount - stripIdxCount;
    int numLooseTris = looseCount / 3;
    for (int i = 0; i < numLooseTris; i++) {
      pos += 3;
      stripEnds.push_back(pos - 1);
    }

    // Sort strip ends for binary search
    std::sort(stripEnds.begin(), stripEnds.end());

    // Write each vertex
    for (uint32_t i = 0; i < mesh->indexCount; i++) {
      uint32_t originalIdx = mesh->indices[i];
      const Vertex &srcV = mesh->vertices[originalIdx];

      // Determine flag: EOL if this is the last vertex of any strip
      uint32_t flag = 0xE0000000; // PVR_CMD_VERTEX
      if (std::binary_search(stripEnds.begin(), stripEnds.end(), i)) {
        flag = 0xF0000000; // PVR_CMD_VERTEX_EOL
      }

      if (!isAnimated) {
        StaticVertex sv;
        sv.x = srcV.x;
        sv.y = srcV.y;
        sv.z = srcV.z;
        sv.u = srcV.u;
        sv.v = srcV.v;
        sv.argb = (srcV.a << 24) | (srcV.r << 16) | (srcV.g << 8) | srcV.b;
        sv.nx = srcV.nx;
        sv.ny = srcV.ny;
        sv.nz = srcV.nz;
        sv.pad = 0;
        sv.flags = flag;
        fwrite(&sv, sizeof(StaticVertex), 1, file);
      } else {
        StaticVertex sv;
        sv.x = srcV.x;
        sv.y = srcV.y;
        sv.z = srcV.z;
        sv.u = srcV.u;
        sv.v = srcV.v;
        sv.argb = (srcV.a << 24) | (srcV.r << 16) | (srcV.g << 8) | srcV.b;
        sv.nx = srcV.nx;
        sv.ny = srcV.ny;
        sv.nz = srcV.nz;
        sv.pad = srcV.boneId;
        sv.flags = flag;
        fwrite(&sv, sizeof(StaticVertex), 1, file);
      }
    }
  }

  // ---- Embed textures (v5) ----
  // Derive output directory from filename
  char outputDir[256] = ".";
  strncpy(outputDir, filename, sizeof(outputDir) - 1);
  char *lastSlash = strrchr(outputDir, '/');
  if (!lastSlash) lastSlash = strrchr(outputDir, '\\');
  if (lastSlash) *lastSlash = '\0';
  else strcpy(outputDir, ".");

  // Count how many .dt files exist
  uint32_t tex_count = 0;
  for (const auto &pair : textureNames) {
    if (pair.first + 1 > (int)tex_count) tex_count = pair.first + 1;
  }
  // Also check for .dt files that might exist without being in textureNames
  for (uint32_t i = 0; i < 256; i++) {
    char dtPath[512];
    snprintf(dtPath, sizeof(dtPath), "%s/texture_%u.dt", outputDir, i);
    FILE *probe = fopen(dtPath, "rb");
    if (probe) {
      fclose(probe);
      if (i + 1 > tex_count) tex_count = i + 1;
    } else {
      if (i >= tex_count) break;
    }
  }

  printf("\n=== Embedding %u textures ===\n", tex_count);
  fwrite(&tex_count, sizeof(uint32_t), 1, file);

  struct TexTableEntry { uint32_t offset; uint32_t size; };
  std::vector<TexTableEntry> texTable(tex_count, {0, 0});

  // Reserve space for the texture table — we'll seek back to fill it
  long tablePos = ftell(file);
  fwrite(texTable.data(), sizeof(TexTableEntry), tex_count, file);

  for (uint32_t i = 0; i < tex_count; i++) {
    char dtPath[512];
    snprintf(dtPath, sizeof(dtPath), "%s/texture_%u.dt", outputDir, i);
    FILE *dtf = fopen(dtPath, "rb");
    if (!dtf) {
      printf("  Texture %u: not found (%s), skipping\n", i, dtPath);
      continue;
    }

    fseek(dtf, 0, SEEK_END);
    uint32_t sz = (uint32_t)ftell(dtf);
    fseek(dtf, 0, SEEK_SET);

    texTable[i].offset = (uint32_t)ftell(file);
    texTable[i].size = sz;

    // Stream copy
    uint8_t buf[4096];
    uint32_t remaining = sz;
    while (remaining > 0) {
      size_t chunk = remaining < 4096 ? remaining : 4096;
      fread(buf, 1, chunk, dtf);
      fwrite(buf, 1, chunk, file);
      remaining -= chunk;
    }
    fclose(dtf);
    remove(dtPath);  // clean up loose .dt file

    printf("  Texture %u: %u bytes embedded, .dt removed\n", i, sz);
  }

  // Seek back and write completed table
  long endPos = ftell(file);
  fseek(file, tablePos, SEEK_SET);
  fwrite(texTable.data(), sizeof(TexTableEntry), tex_count, file);
  fseek(file, endPos, SEEK_SET);

  printf("=== Texture embedding complete ===\n");

  long finalPos = ftell(file);
  fclose(file);
  printf("File writing complete at %ld bytes\n", finalPos);
}
