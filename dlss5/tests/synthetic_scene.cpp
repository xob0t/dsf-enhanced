// SPDX-License-Identifier: MIT

#include "synthetic_scene.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace synthetic {

namespace {

struct V3 {
  float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Normalize(V3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }

constexpr float kNear = 0.1f;
constexpr float kFar = 400.0f;
constexpr float kTanHalfFov = 0.5773503f;  // 60 degrees vertical

struct Camera {
  V3 position;
  float yaw;  // radians, 0 looks down +z
  V3 right, up, forward;
};

Camera CameraAt(int frame) {
  const float t = static_cast<float>(frame);
  Camera c;
  c.position = {1.2f * std::sin(t * 0.07f), 1.4f, -20.0f + t * 0.35f};
  c.yaw = 0.12f * std::sin(t * 0.05f);
  c.forward = {std::sin(c.yaw), -0.04f, std::cos(c.yaw)};
  c.forward = Normalize(c.forward);
  c.right = Normalize(V3{c.forward.z, 0.0f, -c.forward.x});
  c.up = {c.forward.y * c.right.z - c.forward.z * c.right.y, c.forward.z * c.right.x - c.forward.x * c.right.z,
          c.forward.x * c.right.y - c.forward.y * c.right.x};
  return c;
}

struct Box {
  V3 lo, hi;
  V3 color;
  bool windows;
};
struct Sphere {
  V3 center;
  float radius;
  V3 color;
  float gloss;
};

struct Scene {
  std::vector<Box> boxes;
  std::vector<Sphere> spheres;
};

const Scene& GetScene() {
  static const Scene scene = [] {
    Scene s;
    // Buildings along both sides of a street that runs along +z.
    const V3 palette[] = {{0.62f, 0.52f, 0.42f}, {0.45f, 0.47f, 0.52f}, {0.70f, 0.66f, 0.58f}, {0.55f, 0.36f, 0.30f}};
    for (int i = 0; i < 14; ++i) {
      const float z = -24.0f + i * 9.0f;
      const float hl = 8.0f + 6.0f * std::fabs(std::sin(i * 1.7f));
      const float hr = 7.0f + 7.0f * std::fabs(std::cos(i * 1.3f));
      s.boxes.push_back({{-16.0f, 0.0f, z}, {-7.0f, hl, z + 8.0f}, palette[i % 4], true});
      s.boxes.push_back({{7.0f, 0.0f, z + 1.0f}, {16.0f, hr, z + 8.5f}, palette[(i + 2) % 4], true});
    }
    // Kerbs.
    s.boxes.push_back({{-7.0f, 0.0f, -40.0f}, {-4.5f, 0.15f, 120.0f}, {0.55f, 0.55f, 0.53f}, false});
    s.boxes.push_back({{4.5f, 0.0f, -40.0f}, {7.0f, 0.15f, 120.0f}, {0.55f, 0.55f, 0.53f}, false});
    // A car-sized block and a few glossy spheres.
    s.boxes.push_back({{-3.2f, 0.25f, 4.0f}, {-1.4f, 1.6f, 8.5f}, {0.60f, 0.08f, 0.07f}, false});
    s.spheres.push_back({{2.2f, 0.9f, 2.0f}, 0.9f, {0.85f, 0.75f, 0.25f}, 0.6f});
    s.spheres.push_back({{-5.6f, 1.3f, 14.0f}, 0.7f, {0.2f, 0.4f, 0.8f}, 0.3f});
    s.spheres.push_back({{3.0f, 1.1f, 22.0f}, 1.1f, {0.9f, 0.9f, 0.9f}, 0.9f});
    return s;
  }();
  return scene;
}

struct Hit {
  float t = 1e30f;
  V3 normal{0, 1, 0};
  V3 albedo{0, 0, 0};
  float gloss = 0.0f;
  int kind = -1;  // 0 ground, 1 box, 2 sphere
};

bool IntersectBox(const Box& b, V3 o, V3 d, float* tNear, V3* normal) {
  float t0 = 0.0f;
  float t1 = 1e30f;
  int axis = -1;
  float sign = 1.0f;
  const float lo[3] = {b.lo.x, b.lo.y, b.lo.z};
  const float hi[3] = {b.hi.x, b.hi.y, b.hi.z};
  const float oo[3] = {o.x, o.y, o.z};
  const float dd[3] = {d.x, d.y, d.z};
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(dd[i]) < 1e-8f) {
      if (oo[i] < lo[i] || oo[i] > hi[i]) return false;
      continue;
    }
    float a = (lo[i] - oo[i]) / dd[i];
    float c = (hi[i] - oo[i]) / dd[i];
    float s = -1.0f;
    if (a > c) {
      std::swap(a, c);
      s = 1.0f;
    }
    if (a > t0) {
      t0 = a;
      axis = i;
      sign = s;
    }
    t1 = std::min(t1, c);
    if (t0 > t1) return false;
  }
  if (axis < 0) return false;
  *tNear = t0;
  *normal = axis == 0 ? V3{sign, 0, 0} : (axis == 1 ? V3{0, sign, 0} : V3{0, 0, sign});
  return true;
}

Hit Trace(V3 o, V3 d) {
  const Scene& scene = GetScene();
  Hit hit;
  if (d.y < -1e-6f) {
    const float t = -o.y / d.y;
    if (t > 0.0f && t < hit.t) {
      hit.t = t;
      hit.normal = {0, 1, 0};
      hit.kind = 0;
    }
  }
  for (const Box& b : scene.boxes) {
    float t;
    V3 n;
    if (IntersectBox(b, o, d, &t, &n) && t > 1e-4f && t < hit.t) {
      hit.t = t;
      hit.normal = n;
      hit.kind = 1;
      const V3 p = o + d * t;
      V3 albedo = b.color;
      if (b.windows && std::fabs(n.y) < 0.5f) {
        const float u = std::fabs(n.x) > 0.5f ? p.z : p.x;
        const bool window = std::fmod(std::fabs(u) + 0.3f, 2.0f) < 1.1f && std::fmod(p.y + 0.2f, 3.0f) < 1.6f && p.y > 1.5f;
        if (window) {
          albedo = {0.10f, 0.13f, 0.17f};
          hit.gloss = 0.8f;
        }
      }
      hit.albedo = albedo;
    }
  }
  for (const Sphere& s : scene.spheres) {
    const V3 oc = o - s.center;
    const float b = Dot(oc, d);
    const float c = Dot(oc, oc) - s.radius * s.radius;
    const float disc = b * b - c;
    if (disc > 0.0f) {
      const float t = -b - std::sqrt(disc);
      if (t > 1e-4f && t < hit.t) {
        hit.t = t;
        hit.normal = Normalize(o + d * t - s.center);
        hit.albedo = s.color;
        hit.gloss = s.gloss;
        hit.kind = 2;
      }
    }
  }
  if (hit.kind == 0) {
    const V3 p = o + d * hit.t;
    // Asphalt with a centre line and lane edges; pavement beyond the kerbs.
    float noise = 0.04f * std::sin(p.x * 7.1f + p.z * 3.3f) * std::sin(p.z * 5.7f - p.x * 2.1f);
    V3 albedo = {0.16f + noise, 0.16f + noise, 0.17f + noise};
    if (std::fabs(p.x) < 0.08f && std::fmod(p.z + 100.0f, 4.0f) < 2.2f) albedo = {0.85f, 0.75f, 0.30f};
    if (std::fabs(std::fabs(p.x) - 4.2f) < 0.07f) albedo = {0.8f, 0.8f, 0.8f};
    if (std::fabs(p.x) > 7.0f) albedo = {0.42f, 0.41f, 0.39f};
    hit.albedo = albedo;
    hit.gloss = 0.1f;
  }
  return hit;
}

const V3 kSunDir = Normalize(V3{-0.45f, 0.75f, 0.35f});

V3 Sky(V3 d) {
  const float h = std::max(d.y, 0.0f);
  V3 sky = V3{0.55f, 0.70f, 0.92f} * (1.0f - h) + V3{0.22f, 0.40f, 0.78f} * h;
  const float sun = std::pow(std::max(Dot(d, kSunDir), 0.0f), 600.0f);
  return sky + V3{4.0f, 3.6f, 3.0f} * sun;
}

V3 Shade(V3 o, V3 d, int depth) {
  const Hit hit = Trace(o, d);
  if (hit.kind < 0) return Sky(d);
  const V3 p = o + d * hit.t + hit.normal * 1e-3f;
  const Hit shadow = Trace(p, kSunDir);
  const float lit = shadow.kind < 0 ? 1.0f : 0.0f;
  const float diffuse = std::max(Dot(hit.normal, kSunDir), 0.0f) * lit;
  const float skyLight = 0.35f + 0.25f * hit.normal.y;
  V3 color = hit.albedo * (V3{1.0f, 0.95f, 0.85f} * (2.2f * diffuse) + V3{0.45f, 0.55f, 0.75f} * skyLight);
  if (hit.gloss > 0.0f && depth == 0) {
    const V3 r = d - hit.normal * (2.0f * Dot(d, hit.normal));
    const float fresnel = 0.04f + 0.96f * std::pow(1.0f - std::max(-Dot(d, hit.normal), 0.0f), 5.0f);
    color = color * (1.0f - fresnel * hit.gloss) + Shade(p, r, 1) * (fresnel * hit.gloss);
  }
  return color;
}

float ToneMap(float x) {
  // ACES fit followed by sRGB encoding: the same SDR, display-encoded domain Remix feeds.
  x = std::max(x, 0.0f) * 0.8f;
  const float y = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
  const float c = std::clamp(y, 0.0f, 1.0f);
  return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Projects a world point with camera c into continuous pixel coordinates and depth.
bool Project(const Camera& c, V3 p, uint32_t width, uint32_t height, float* px, float* py, float* depth) {
  const V3 rel = p - c.position;
  const float z = Dot(rel, c.forward);
  if (z <= 1e-4f) return false;
  const float aspect = static_cast<float>(width) / static_cast<float>(height);
  const float ndcX = Dot(rel, c.right) / (z * kTanHalfFov * aspect);
  const float ndcY = Dot(rel, c.up) / (z * kTanHalfFov);
  *px = (ndcX + 1.0f) * 0.5f * width;
  *py = (1.0f - ndcY) * 0.5f * height;
  *depth = kFar / (kFar - kNear) - (kFar * kNear / (kFar - kNear)) / z;
  return true;
}

}  // namespace

void HaltonJitter(int index, float* x, float* y) {
  auto halton = [](int i, int base) {
    float f = 1.0f;
    float r = 0.0f;
    for (int n = i + 1; n > 0; n /= base) {
      f /= base;
      r += f * (n % base);
    }
    return r;
  };
  *x = halton(index, 2) - 0.5f;
  *y = halton(index, 3) - 0.5f;
}

void Render(uint32_t width, uint32_t height, int frame, float jitterX, float jitterY, Frame* out) {
  out->width = width;
  out->height = height;
  out->color.assign(static_cast<size_t>(width) * height * 4, 1.0f);
  out->depth.assign(static_cast<size_t>(width) * height, 1.0f);
  out->motion.assign(static_cast<size_t>(width) * height * 2, 0.0f);
  const Camera cur = CameraAt(frame);
  const Camera prev = CameraAt(frame - 1);
  const float aspect = static_cast<float>(width) / static_cast<float>(height);

  auto rows = [&](uint32_t y0, uint32_t y1) {
    for (uint32_t y = y0; y < y1; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const float sx = x + 0.5f - jitterX;
        const float sy = y + 0.5f - jitterY;
        const float ndcX = 2.0f * sx / width - 1.0f;
        const float ndcY = 1.0f - 2.0f * sy / height;
        const V3 d = Normalize(cur.forward + cur.right * (ndcX * kTanHalfFov * aspect) + cur.up * (ndcY * kTanHalfFov));
        const V3 c = Shade(cur.position, d, 0);
        const size_t i = static_cast<size_t>(y) * width + x;
        out->color[i * 4 + 0] = ToneMap(c.x);
        out->color[i * 4 + 1] = ToneMap(c.y);
        out->color[i * 4 + 2] = ToneMap(c.z);

        const Hit hit = Trace(cur.position, d);
        // Sky reprojects as a direction: a point far beyond the scene.
        const V3 world = hit.kind < 0 ? cur.position + d * 1.0e4f : cur.position + d * hit.t;
        float px, py, depth;
        if (hit.kind >= 0 && Project(cur, world, width, height, &px, &py, &depth)) out->depth[i] = depth;
        float ppx, ppy, pdepth;
        if (Project(prev, world, width, height, &ppx, &ppy, &pdepth)) {
          out->motion[i * 2 + 0] = ppx - (x + 0.5f);
          out->motion[i * 2 + 1] = ppy - (y + 0.5f);
        }
      }
    }
  };
  const uint32_t threads = std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  for (uint32_t t = 0; t < threads; ++t) {
    pool.emplace_back(rows, height * t / threads, height * (t + 1) / threads);
  }
  for (std::thread& t : pool) t.join();
}

}  // namespace synthetic
