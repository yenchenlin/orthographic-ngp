/*
 * Copyright (c) 2020-2022, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

/** @file   testbed.h
 *  @author Thomas Müller & Alex Evans, NVIDIA
 */

#pragma once

#include <neural-graphics-primitives/adam_optimizer.h>
#include <neural-graphics-primitives/camera_path.h>
#include <neural-graphics-primitives/common.h>
#include <neural-graphics-primitives/discrete_distribution.h>
#include <neural-graphics-primitives/nerf.h>
#include <neural-graphics-primitives/nerf_loader.h>
#include <neural-graphics-primitives/render_buffer.h>
#include <neural-graphics-primitives/trainable_buffer.cuh>

#include <tiny-cuda-nn/cuda_graph.h>
#include <tiny-cuda-nn/random.h>

#include <json/json.hpp>

#include <filesystem/path.h>

#ifdef NGP_PYTHON
#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#endif

struct GLFWwindow;

TCNN_NAMESPACE_BEGIN
template <typename T> class Loss;
template <typename T> class Optimizer;
template <typename T> class Encoding;
template <typename T, typename PARAMS_T> class Network;
template <typename T, typename PARAMS_T, typename COMPUTE_T> class Trainer;
template <uint32_t N_DIMS, uint32_t RANK, typename T> class TrainableBuffer;
TCNN_NAMESPACE_END

NGP_NAMESPACE_BEGIN

template <typename T> class NerfNetwork;
class TriangleOctree;
class TriangleBvh;
struct Triangle;
class GLTexture;

class Testbed {
public:
	Testbed(ETestbedMode mode);
	~Testbed();
	Testbed(ETestbedMode mode, const std::string& data_path) : Testbed(mode) { load_training_data(data_path); }
	Testbed(ETestbedMode mode, const std::string& data_path, const std::string& network_config_path) : Testbed(mode, data_path) { reload_network_from_file(network_config_path); }
	Testbed(ETestbedMode mode, const std::string& data_path, const nlohmann::json& network_config) : Testbed(mode, data_path) { reload_network_from_json(network_config); }
	void load_training_data(const std::string& data_path);
	void clear_training_data();

	using distance_fun_t = std::function<void(uint32_t, const tcnn::GPUMemory<Eigen::Vector3f>&, tcnn::GPUMemory<float>&, cudaStream_t)>;
	using normals_fun_t = std::function<void(uint32_t, const tcnn::GPUMemory<Eigen::Vector3f>&, tcnn::GPUMemory<Eigen::Vector3f>&, cudaStream_t)>;

	class NerfTracer {
	public:
		NerfTracer() : m_hit_counter(1), m_alive_counter(1) {}

		void init_rays_from_camera(
			uint32_t spp,
			uint32_t padded_output_width,
			const Eigen::Vector2i& resolution,
			const Eigen::Vector2f& focal_length,
			const Eigen::Matrix<float, 3, 4>& camera_matrix0,
			const Eigen::Matrix<float, 3, 4>& camera_matrix1,
			Eigen::Vector2f screen_center,
			bool snap_to_pixel_centers,
			const BoundingBox& render_aabb,
			float plane_z,
			float dof,
			const CameraDistortion& camera_distortion,
			const float* envmap_data,
			const Eigen::Vector2i& envmap_resolution,
			const float* distortion_data,
			const Eigen::Vector2i& distortion_resolution,
			Eigen::Array4f* frame_buffer,
			uint8_t *grid,
			int show_accel,
			float cone_angle_constant,
			ERenderMode render_mode,
			cudaStream_t stream
		);

		uint32_t trace(
			NerfNetwork<precision_t>& network,
			const BoundingBox& render_aabb,
			const BoundingBox& train_aabb,
			const uint32_t n_training_images,
			const Eigen::Matrix<float, 3, 4>* training_xforms,
			const Eigen::Vector2f& focal_length,
			float cone_angle_constant,
			const uint8_t* grid,
			ERenderMode render_mode,
			const Eigen::Matrix<float, 3, 4> &camera_matrix,
			float depth_scale,
			int visualized_layer,
			int visualized_dim,
			ENerfActivation rgb_activation,
			ENerfActivation density_activation,
			int show_accel,
			float min_alpha,
			cudaStream_t stream
		);

		void enlarge(size_t n_elements, uint32_t padded_output_width, cudaStream_t stream);
		RaysNerfSoa& rays_hit() { return m_rays_hit; }
		RaysNerfSoa& rays_init() { return m_rays[0]; }
		uint32_t n_rays_initialized() const { return m_n_rays_initialized; }

		void clear() {
			m_scratch_alloc = {};
		}

	private:
		RaysNerfSoa m_rays[2];
		RaysNerfSoa m_rays_hit;
		precision_t* m_network_output;
		NerfCoordinate* m_network_input;
		tcnn::GPUMemory<uint32_t> m_hit_counter;
		tcnn::GPUMemory<uint32_t> m_alive_counter;
		uint32_t m_n_rays_initialized = 0;
		tcnn::GPUMemoryArena::Allocation m_scratch_alloc;
	};

	class FiniteDifferenceNormalsApproximator {
	public:
		void enlarge(uint32_t n_elements);
		void normal(uint32_t n_elements, const distance_fun_t& distance_function, tcnn::GPUMemory<Eigen::Vector3f>& pos, tcnn::GPUMemory<Eigen::Vector3f>& normal, float epsilon, cudaStream_t stream);

	private:
		tcnn::GPUMemory<Eigen::Vector3f> dx;
		tcnn::GPUMemory<Eigen::Vector3f> dy;
		tcnn::GPUMemory<Eigen::Vector3f> dz;

		tcnn::GPUMemory<float> dist_dx_pos;
		tcnn::GPUMemory<float> dist_dy_pos;
		tcnn::GPUMemory<float> dist_dz_pos;

		tcnn::GPUMemory<float> dist_dx_neg;
		tcnn::GPUMemory<float> dist_dy_neg;
		tcnn::GPUMemory<float> dist_dz_neg;
	};

	struct LevelStats {
		float mean() { return count ? (x / (float)count) : 0.f; }
		float variance() { return count ? (xsquared - (x * x) / (float)count) / (float)count : 0.f; }
		float sigma() { return sqrtf(variance()); }
		float fraczero() { return (float)numzero / float(count + numzero); }
		float fracquant() { return (float)numquant / float(count); }

		float x;
		float xsquared;
		float min;
		float max;
		int numzero;
		int numquant;
		int count;
	};

	static constexpr float LOSS_SCALE = 128.f;

	void render_volume(CudaRenderBuffer& render_buffer,
		const Eigen::Vector2f& focal_length,
		const Eigen::Matrix<float, 3, 4>& camera_matrix,
		const Eigen::Vector2f& screen_center,
		cudaStream_t stream
	);
	void train_volume(size_t target_batch_size, size_t n_steps, cudaStream_t stream);
	void training_prep_volume(uint32_t batch_size, uint32_t n_training_steps, cudaStream_t stream) {}
	void load_volume();

	void render_sdf(
		const distance_fun_t& distance_function,
		const normals_fun_t& normals_function,
		CudaRenderBuffer& render_buffer,
		const Eigen::Vector2i& max_res,
		const Eigen::Vector2f& focal_length,
		const Eigen::Matrix<float, 3, 4>& camera_matrix,
		const Eigen::Vector2f& screen_center,
		cudaStream_t stream
	);
	void render_nerf(CudaRenderBuffer& render_buffer, const Eigen::Vector2i& max_res, const Eigen::Vector2f& focal_length, const Eigen::Matrix<float, 3, 4>& camera_matrix0, const Eigen::Matrix<float, 3, 4>& camera_matrix1, const Eigen::Vector2f& screen_center, cudaStream_t stream);
	void render_image(CudaRenderBuffer& render_buffer, cudaStream_t stream);
	void render_frame(const Eigen::Matrix<float, 3, 4>& camera_matrix0, const Eigen::Matrix<float, 3, 4>& camera_matrix1, CudaRenderBuffer& render_buffer, bool to_srgb = true) ;
	void visualize_nerf_cameras(const Eigen::Matrix<float, 4, 4>& world2proj);
	nlohmann::json load_network_config(const filesystem::path& network_config_path);
	void reload_network_from_file(const std::string& network_config_path);
	void reload_network_from_json(const nlohmann::json& json, const std::string& config_base_path=""); // config_base_path is needed so that if the passed in json uses the 'parent' feature, we know where to look... be sure to use a filename, or if a directory, end with a trailing slash
	void reset_accumulation();
	static ELossType string_to_loss_type(const std::string& str);
	void reset_network();
	void update_nerf_focal_lengths();
	void update_nerf_transforms();
	void load_nerf();
	void load_mesh();
	void set_exposure(float exposure) { m_exposure = exposure; }
	void set_max_level(float maxlevel);
	void set_min_level(float minlevel);
	void set_visualized_dim(int dim);
	void set_visualized_layer(int layer);
	void translate_camera(const Eigen::Vector3f& rel);
	void mouse_drag(const Eigen::Vector2f& rel, int button);
	void mouse_wheel(Eigen::Vector2f m, float delta);
	void handle_file(const std::string& file);
	void set_nerf_camera_matrix(const Eigen::Matrix<float, 3, 4>& cam);
	Eigen::Vector3f look_at() const;
	void set_look_at(const Eigen::Vector3f& pos);
	float scale() const { return m_scale; }
	void set_scale(float scale);
	Eigen::Vector3f view_pos() const { return m_camera.col(3); }
	Eigen::Vector3f view_dir() const { return m_camera.col(2); }
	Eigen::Vector3f view_up() const { return m_camera.col(1); }
	Eigen::Vector3f view_side() const { return m_camera.col(0); }
	void set_view_dir(const Eigen::Vector3f& dir);
	void set_camera_to_training_view(int trainview);
	void reset_camera();
	bool keyboard_event();
	void generate_training_samples_sdf(Eigen::Vector3f* positions, float* distances, uint32_t n_to_generate, cudaStream_t stream, bool uniform_only);
	void update_density_grid_nerf(float decay, uint32_t n_uniform_density_grid_samples, uint32_t n_nonuniform_density_grid_samples, cudaStream_t stream);
	void update_density_grid_mean_and_bitfield(cudaStream_t stream);
	void train_nerf(uint32_t target_batch_size, uint32_t n_training_steps, cudaStream_t stream);
	void train_nerf_step(uint32_t target_batch_size, uint32_t n_rays_per_batch, uint32_t* counter, uint32_t* compacted_counter, float* loss, cudaStream_t stream);
	void train_sdf(size_t target_batch_size, size_t n_steps, cudaStream_t stream);
	void train_image(size_t target_batch_size, size_t n_steps, cudaStream_t stream);
	void set_train(bool mtrain);
	void imgui();
	void training_prep_nerf(uint32_t batch_size, uint32_t n_training_steps, cudaStream_t stream);
	void training_prep_sdf(uint32_t batch_size, uint32_t n_training_steps, cudaStream_t stream);
	void training_prep_image(uint32_t batch_size, uint32_t n_training_steps, cudaStream_t stream) {}
	void train(uint32_t n_training_steps, uint32_t batch_size);
	Eigen::Vector2f calc_focal_length(const Eigen::Vector2i& resolution, int fov_axis, float zoom) const ;
	Eigen::Vector2f render_screen_center() const ;
	void optimise_mesh_step(uint32_t N_STEPS);
	void compute_mesh_vertex_colors();
	tcnn::GPUMemory<float> get_density_on_grid(Eigen::Vector3i res3d, const BoundingBox& aabb);
	tcnn::GPUMemory<Eigen::Array4f> get_rgba_on_grid(Eigen::Vector3i res3d, Eigen::Vector3f ray_dir);
	int marching_cubes(Eigen::Vector3i res3d, const BoundingBox& aabb, float thresh);

	// Determines the 3d focus point by rendering a little 16x16 depth image around
	// the mouse cursor and picking the median depth.
	void determine_autofocus_target_from_pixel(const Eigen::Vector2i& focus_pixel);
	void autofocus();
	size_t n_params();
	size_t first_encoder_param();
	size_t n_encoding_params();

#ifdef NGP_PYTHON
	pybind11::dict compute_marching_cubes_mesh(Eigen::Vector3i res3d = Eigen::Vector3i::Constant(128), BoundingBox aabb = BoundingBox{Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}, float thresh=2.5f);
	pybind11::array_t<float> render_to_cpu(int width, int height, int spp, bool linear, float start_t, float end_t, float fps, float shutter_fraction);
	pybind11::array_t<float> screenshot(bool linear) const;
	void override_sdf_training_data(pybind11::array_t<float> points, pybind11::array_t<float> distances);
#endif

	double calculate_iou(uint32_t n_samples=128*1024*1024, float scale_existing_results_factor=0.0, bool blocking=true, bool force_use_octree = true);
	void draw_visualizations(const Eigen::Matrix<float, 3, 4>& camera_matrix);
	void draw_contents();
	filesystem::path training_data_path() const;
	void init_window(int resw, int resh, bool hidden = false);
	void destroy_window();
	void apply_camera_smoothing(float elapsed_ms);
	int find_best_training_view(int default_view);
	bool handle_user_input();
	void gather_histograms();
	void draw_gui();
	bool frame();
	bool want_repl();
	void load_image();
	void load_exr_image();
	void load_stbi_image();
	void load_binary_image();
	uint32_t n_dimensions_to_visualize() const;
	float fov() const ;
	void set_fov(float val) ;
	Eigen::Vector2f fov_xy() const ;
	void set_fov_xy(const Eigen::Vector2f& val);
	void save_snapshot(const std::string& filepath_string, bool include_optimizer_state);
	void load_snapshot(const std::string& filepath_string);
	CameraKeyframe copy_camera_to_keyframe() const;
	void set_camera_from_keyframe(const CameraKeyframe& k);
	void set_camera_from_time(float t);
	void update_loss_graph();
	void load_camera_path(const std::string& filepath_string);

	float compute_image_mse();

	void compute_and_save_marching_cubes_mesh(const char* filename, Eigen::Vector3i res3d = Eigen::Vector3i::Constant(128), BoundingBox aabb = {}, float thresh = 2.5f, bool unwrap_it = false);

	////////////////////////////////////////////////////////////////
	// marching cubes related state
	struct MeshState {
		float thresh = 2.5f;
		int res = 256;
		bool unwrap = false;
		float smooth_amount = 2048.f;
		float density_amount = 128.f;
		float inflate_amount = 1.f;
		bool optimize_mesh = false;
		tcnn::GPUMemory<Eigen::Vector3f> verts;
		tcnn::GPUMemory<Eigen::Vector3f> vert_normals;
		tcnn::GPUMemory<Eigen::Vector3f> vert_colors;
		tcnn::GPUMemory<Eigen::Vector4f> verts_smoothed; // homogenous
		tcnn::GPUMemory<uint32_t> indices;
		tcnn::GPUMemory<Eigen::Vector3f> verts_gradient;
		std::shared_ptr<TrainableBuffer<3, 1, float>> trainable_verts;
		std::shared_ptr<tcnn::Optimizer<float>> verts_optimizer;

		void clear() {
			indices={};
			verts={};
			vert_normals={};
			vert_colors={};
			verts_smoothed={};
			verts_gradient={};
			trainable_verts=nullptr;
			verts_optimizer=nullptr;
		}
	};
	MeshState m_mesh;

	bool m_want_repl = false;

	bool m_render_window = false;
	bool m_gather_histograms = false;

	bool m_include_optimizer_state_in_snapshot = false;
	bool m_render_ground_truth = false;
	bool m_train = false;
	bool m_training_data_available = false;
	bool m_render = true;
	int m_max_spp = 0;
	ETestbedMode m_testbed_mode = ETestbedMode::Sdf;
	bool m_max_level_rand_training = false;

	// Rendering stuff
	Eigen::Vector2i m_window_res = Eigen::Vector2i::Constant(0);
	bool m_dynamic_res=true;
	int m_fixed_res_factor=8;
	float m_last_render_res_factor = 1.0f;
	float m_scale = 1;
	float m_dof = 0.0f;
	Eigen::Vector2f m_relative_focal_length = Eigen::Vector2f::Ones();
	uint32_t m_fov_axis = 1;
	float m_zoom = 1.f; // 2d zoom factor (for insets?)
	Eigen::Vector2f m_screen_center = Eigen::Vector2f::Constant(0.5f); // center of 2d zoom

	Eigen::Matrix<float, 3, 4> m_camera = Eigen::Matrix<float, 3, 4>::Zero();
	Eigen::Matrix<float, 3, 4> m_smoothed_camera = Eigen::Matrix<float, 3, 4>::Zero();
	bool m_fps_camera = false;
	bool m_camera_smoothing = false;
	bool m_autofocus = false;
	Eigen::Vector3f m_autofocus_target = Eigen::Vector3f::Constant(0.5f);

	CameraPath m_camera_path = {};

	Eigen::Vector3f m_up_dir = {0.0f, 1.0f, 0.0f};
	Eigen::Vector3f m_sun_dir = Eigen::Vector3f::Ones().normalized();
	float m_bounding_radius = 1;
	float m_exposure = 0.f;

	ERenderMode m_render_mode = ERenderMode::Shade;
	EMeshRenderMode m_mesh_render_mode = EMeshRenderMode::VertexNormals;

	uint32_t m_seed = 1337;

#ifdef NGP_GUI
	GLFWwindow* m_glfw_window = nullptr;
	std::shared_ptr<GLTexture> m_pip_render_texture;
	std::vector<std::shared_ptr<GLTexture>> m_render_textures;
#endif

	std::vector<CudaRenderBuffer> m_render_surfaces;
	std::unique_ptr<CudaRenderBuffer> m_pip_render_surface;

	struct Nerf {
		NerfTracer tracer;

		struct Training {
			NerfDataset dataset;
			Eigen::Vector2i image_resolution;
			int n_images = 0; // how many images

			struct ErrorMap {
				tcnn::GPUMemory<float> data;
				tcnn::GPUMemory<float> cdf_x_cond_y;
				tcnn::GPUMemory<float> cdf_y;
				tcnn::GPUMemory<float> cdf_img;
				std::vector<float> pmf_img_cpu;
				Eigen::Vector2i resolution = {16, 16};
				Eigen::Vector2i cdf_resolution = {16, 16};
				bool is_cdf_valid = false;
			} error_map;

			std::vector<Eigen::Vector2f> focal_lengths;
			tcnn::GPUMemory<Eigen::Vector2f> focal_lengths_gpu;

			std::vector<Eigen::Matrix<float, 3, 4>> transforms;
			tcnn::GPUMemory<Eigen::Matrix<float, 3, 4>> transforms_gpu;

			std::vector<Eigen::Vector3f> cam_pos_gradient;
			tcnn::GPUMemory<Eigen::Vector3f> cam_pos_gradient_gpu;

			std::vector<Eigen::Vector3f> cam_rot_gradient;
			tcnn::GPUMemory<Eigen::Vector3f> cam_rot_gradient_gpu;

			tcnn::GPUMemory<Eigen::Array3f> cam_exposure_gpu;
			std::vector<Eigen::Array3f> cam_exposure_gradient;
			tcnn::GPUMemory<Eigen::Array3f> cam_exposure_gradient_gpu;

			Eigen::Vector2f cam_focal_length_gradient = Eigen::Vector2f::Zero();
			tcnn::GPUMemory<Eigen::Vector2f> cam_focal_length_gradient_gpu;

			std::vector<AdamOptimizer<Eigen::Array3f>> cam_exposure;
			std::vector<AdamOptimizer<Eigen::Vector3f>> cam_pos_offset;
			std::vector<RotationAdamOptimizer> cam_rot_offset;
			AdamOptimizer<Eigen::Vector2f> cam_focal_length_offset = AdamOptimizer<Eigen::Vector2f>(0.f);

			tcnn::GPUMemory<uint32_t> numsteps_counter; // number of steps each ray took
			tcnn::GPUMemory<uint32_t> numsteps_counter_compacted; // number of steps each ray took
			tcnn::GPUMemory<uint32_t> ray_counter;
			tcnn::GPUMemory<float> loss;

			uint32_t rays_per_batch = 1<<12;
			uint32_t n_rays_total = 0;
			uint32_t measured_batch_size = 0;
			uint32_t measured_batch_size_before_compaction = 0;
			bool random_bg_color = true;
			bool linear_colors = false;
			ELossType loss_type = ELossType::L2;
			bool snap_to_pixel_centers = true;

			bool train_envmap = false;

			bool optimize_distortion = false;
			bool optimize_extrinsics = false;
			bool optimize_focal_length = false;
			bool optimize_exposure = false;
			bool render_error_overlay = false;
			float error_overlay_brightness = 0.125f;
			uint32_t n_steps_between_cam_updates = 16;
			uint32_t n_steps_since_cam_update = 0;

			bool sample_focal_plane_proportional_to_error = false;
			bool sample_image_proportional_to_error = false;
			bool include_sharpness_in_error = false;
			uint32_t n_steps_between_error_map_updates = 128;
			uint32_t n_steps_since_error_map_update = 0;
			uint32_t n_rays_since_error_map_update = 0;

			float near_distance = 0.2f;
			float density_grid_decay = 0.95f;
			int view = 0;

			tcnn::GPUMemory<float> sharpness_grid;
		} training = {};

		tcnn::GPUMemory<float> density_grid; // NERF_GRIDSIZE()^3 grid of EMA smoothed densities from the network
		tcnn::GPUMemory<uint8_t> density_grid_bitfield;
		uint8_t* get_density_grid_bitfield_mip(uint32_t mip);
		tcnn::GPUMemory<float> density_grid_mean;
		uint32_t density_grid_ema_step = 0;

		uint32_t max_cascade = 0;

		tcnn::GPUMemory<NerfCoordinate> vis_input;
		tcnn::GPUMemory<Eigen::Array4f> vis_rgba;

		ENerfActivation rgb_activation = ENerfActivation::Exponential;
		ENerfActivation density_activation = ENerfActivation::Exponential;

		int show_accel = -1;

		float sharpen = 0.f;

		float cone_angle_constant = 1.f/256.f;

		bool visualize_cameras = false;
		bool render_with_camera_distortion = false;

		float rendering_min_alpha = 0.01f;
	} m_nerf;

	enum EDataType {
		Float,
		Half,
	};
	struct Image {
		Eigen::Vector2f pos = Eigen::Vector2f::Constant(0.0f);
		tcnn::GPUMemory<char> data;

		EDataType type = EDataType::Float;
		Eigen::Vector2i resolution = Eigen::Vector2i::Constant(0.0f);

		tcnn::GPUMemory<Eigen::Vector2f> render_coords;
		tcnn::GPUMemory<Eigen::Array3f> render_out;

		struct Training {
			tcnn::GPUMemory<float> positions_tmp;
			tcnn::GPUMemory<Eigen::Vector2f> positions;
			tcnn::GPUMemory<Eigen::Array3f> targets;

			bool snap_to_pixel_centers = true;
			bool linear_colors = false;
		} training  = {};

		ERandomMode random_mode = ERandomMode::Stratified;
	} m_image;

	struct VolPayload {
		Eigen::Vector3f dir;
		Eigen::Array4f col;
		uint32_t pixidx;
	};

	struct Volume {
		float albedo = 0.95f;
		float scattering = 0.f;
		float inv_distance_scale = 100.f;
		tcnn::GPUMemory<char> nanovdb_grid;
		tcnn::GPUMemory<uint8_t> bitgrid;
		float global_majorant = 1.f;
		Eigen::Vector3f world2index_offset = {0,0,0};
		float world2index_scale = 1.f;

		struct Training {
			tcnn::GPUMemory<Eigen::Vector3f> positions = {};
			tcnn::GPUMemory<Eigen::Array4f> targets = {};
		} training = {};

		// tracing state
		tcnn::GPUMemory<Eigen::Vector3f> pos[2] = {};
		tcnn::GPUMemory<VolPayload> payload[2] = {};
		tcnn::GPUMemory<uint32_t> hit_counter = {};
		tcnn::GPUMemory<Eigen::Array4f> radiance_and_density;
	} m_volume;

	float m_camera_velocity = 1.0f;
	EColorSpace m_color_space = EColorSpace::Linear;
	ETonemapCurve m_tonemap_curve = ETonemapCurve::Identity;

	// 3D stuff
	float m_slice_plane_z = 0.0f;
	bool m_floor_enable = false;
	inline float get_floor_y() const { return m_floor_enable ? m_aabb.min.y()+0.001f : -10000.f; }
	BoundingBox m_raw_aabb;
	BoundingBox m_aabb;
	BoundingBox m_render_aabb;

	// Rendering/UI bookkeeping
	float m_training_prep_milliseconds = 0;
	float m_training_milliseconds = 0;
	float m_frame_milliseconds = 0;
	std::chrono::time_point<std::chrono::steady_clock> m_last_frame_time_point;
	float m_gui_elapsed_ms = 0;
	Eigen::Array4f m_background_color = {0.0f, 0.0f, 0.0f, 1.0f};

	// Visualization of neuron activations
	int m_visualized_dimension = -1;
	int m_visualized_layer = 0;
	Eigen::Vector2i m_n_views = {1, 1};
	Eigen::Vector2i m_view_size = {1, 1};
	bool m_single_view = true; // Whether a single neuron is visualized, or all in a tiled grid
	float m_picture_in_picture_res = 0.f; // if non zero, requests a small second picture :)

	bool m_imgui_enabled = true; // tab to toggle
	bool m_visualize_unit_cube = false;
	bool m_snap_to_pixel_centers = false;

	// CUDA stuff
	cudaStream_t m_training_stream;
	cudaStream_t m_inference_stream;

	// Hashgrid encoding analysis
	float m_quant_percent = 0.f;
	LevelStats m_level_stats[32] = {};
	int m_num_levels = 0;
	int m_histo_level = 0; // collect a histogram for this level
	int m_base_grid_resolution;
	float m_per_level_scale;
	float m_histo[257] = {};
	float m_histo_scale = 1.f;

	uint32_t m_training_step = 0;
	float m_loss_scalar = 0.f;
	float m_loss_graph[256] = {};
	int m_loss_graph_samples = 0;

	bool m_train_encoding = true;
	bool m_train_network = true;

	filesystem::path m_data_path;
	filesystem::path m_network_config_path;

	nlohmann::json m_network_config;

	default_rng_t m_rng;

	CudaRenderBuffer m_windowless_render_surface{std::make_shared<CudaSurface2D>()};

	uint32_t network_width(uint32_t layer) const;
	uint32_t network_num_forward_activations() const;

	std::shared_ptr<tcnn::Loss<precision_t>> m_loss;
	// Network & training stuff
	std::shared_ptr<tcnn::Optimizer<precision_t>> m_optimizer;
	std::shared_ptr<tcnn::Encoding<precision_t>> m_encoding;
	std::shared_ptr<tcnn::Network<float, precision_t>> m_network;
	std::shared_ptr<tcnn::Trainer<float, precision_t, precision_t>> m_trainer;

	struct TrainableEnvmap {
		std::shared_ptr<tcnn::Optimizer<float>> optimizer;
		std::shared_ptr<TrainableBuffer<4, 2, float>> envmap;
		std::shared_ptr<tcnn::Trainer<float, float, float>> trainer;

		Eigen::Vector2i resolution;
		ELossType loss_type;
	} m_envmap;

	struct TrainableDistortionMap {
		std::shared_ptr<tcnn::Optimizer<float>> optimizer;
		std::shared_ptr<TrainableBuffer<2, 2, float>> map;
		std::shared_ptr<tcnn::Trainer<float, float, float>> trainer;
		Eigen::Vector2i resolution;
	} m_distortion;
	std::shared_ptr<NerfNetwork<precision_t>> m_nerf_network;
};

NGP_NAMESPACE_END
