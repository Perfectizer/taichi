#include "volume_renderer.h"
#include <tbb/tbb.h>
#include <cuda_runtime_api.h>

TLANG_NAMESPACE_BEGIN

bool use_gui = false;

auto volume_renderer = [](std::vector<std::string> cli_param) {
  auto param = parse_param(cli_param);

  bool gpu = param.get("gpu", true);
  TC_P(gpu);
  CoreState::set_trigger_gdb_when_crash(true);
  Program prog(gpu ? Arch::gpu : Arch::x86_64);
  prog.config.print_ir = true;
  TRenderer renderer((Dict()));

  layout([&] { renderer.place_data(); });

  renderer.declare_kernels();

  std::unique_ptr<GUI> gui = nullptr;
  int n = renderer.output_res.y;
  int grid_resolution = renderer.grid_resolution;

  auto f = fopen("snow_density_256.bin", "rb");
  TC_ASSERT_INFO(f, "./snow_density_256.bin not found");
  std::vector<float32> density_field(pow<3>(grid_resolution));
  if (std::fread(density_field.data(), sizeof(float32), density_field.size(),
                 f)) {
  }
  std::fclose(f);

  float32 target_max_density = 724.0;
  auto max_density = 0.0f;
  for (int i = 0; i < pow<3>(grid_resolution); i++) {
    max_density = std::max(max_density, density_field[i]);
  }

  TC_P(max_density);

  for (int i = 0; i < pow<3>(grid_resolution); i++) {
    density_field[i] /= max_density;             // normalize to 1 first
    density_field[i] *= target_max_density * 1;  // then scale
    density_field[i] = std::min(density_field[i], target_max_density);
  }

  for (int i = 0; i < grid_resolution; i++) {
    for (int j = 0; j < grid_resolution; j++) {
      for (int k = 0; k < grid_resolution; k++) {
        auto d = density_field[i * grid_resolution * grid_resolution +
                               j * grid_resolution + k];
        if (d != 0) {  // populate non-empty voxels only
          renderer.density.val<float32>(i, j, k) = d;
        }
      }
    }
  }

  renderer.preprocess_volume();

  float32 exposure = 0;
  float32 exposure_linear = 1;
  float32 gamma = 0.5;
  float SPPS = 0;
  if (use_gui) {
    gui = std::make_unique<GUI>("Volume Renderer", Vector2i(n * 2, n));
    gui->label("Sample/pixel/sec", SPPS);
    gui->slider("depth_limit", renderer.parameters.depth_limit, 1, 20);
    gui->slider("max_density", renderer.parameters.max_density, 1.0f, 2000.0f);
    gui->slider("ground_y", renderer.parameters.ground_y, 0.0f, 0.4f);
    gui->slider("light_y", renderer.parameters.light_y, 0.0f, 4.0f);
    gui->slider("exposure", exposure, -3.0f, 3.0f);
    gui->slider("gamma", gamma, 0.2f, 2.0f);
  }
  Vector2i render_size(n * 2, n);
  Array2D<Vector4> render_buffer;
  render_buffer.initialize(render_size);

  auto tone_map = [&](real x) { return std::pow(x * exposure_linear, gamma); };

  std::vector<float32> buffer(render_size.prod() * 3);

  constexpr int N = 1;
  auto last_time = Time::get_time();
  for (int frame = 0; frame < 1000000; frame++) {
    for (int i = 0; i < N; i++) {
      renderer.sample();
    }
    if (frame % 10 == 0) {
      auto elapsed = Time::get_time() - last_time;
      last_time = Time::get_time();
      SPPS = 1.0f / (elapsed / 10.0f);
      prog.profiler_print();
      prog.profiler_clear();
    }

    real scale = 1.0f / renderer.acc_samples;
    exposure_linear = std::exp(exposure);

    cudaMemcpy(buffer.data(), &renderer.buffer(0).val<float32>(0),
               buffer.size() * sizeof(float32), cudaMemcpyDeviceToHost);
    tbb::parallel_for(0, n * n * 2, [&](int i) {
      render_buffer[i / n][i % n] =
          Vector4(tone_map(scale * buffer[i * 3 + 0]),
                  tone_map(scale * buffer[i * 3 + 1]),
                  tone_map(scale * buffer[i * 3 + 2]), 1.0f);
    });

    if (use_gui) {
      gui->canvas->img = render_buffer;
      gui->update();
    } else {
      std::unique_ptr<Canvas> canvas;
      canvas = std::make_unique<Canvas>(render_buffer);
      canvas->img.write_as_image(fmt::format("{:05d}-{:05d}-{:05d}.png", frame,
                                             N,
                                             renderer.parameters.depth_limit));
    }
  }
};
TC_REGISTER_TASK(volume_renderer);

auto volume_renderer_gui = [](std::vector<std::string> cli_param) {
  use_gui = true;
  volume_renderer(cli_param);
};

TC_REGISTER_TASK(volume_renderer_gui);

TLANG_NAMESPACE_END
