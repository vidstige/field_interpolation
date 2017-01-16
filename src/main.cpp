#include <vector>
#include <random>

#include <imgui/imgui.h>
#include <SDL2/SDL.h>

#include <emilib/dual.hpp>
#include <emilib/gl_lib.hpp>
#include <emilib/gl_lib_opengl.hpp>
#include <emilib/gl_lib_sdl.hpp>
#include <emilib/imgui_gl_lib.hpp>
#include <emilib/imgui_helpers.hpp>
#include <emilib/imgui_sdl.hpp>
#include <emilib/irange.hpp>
#include <emilib/marching_squares.hpp>
#include <emilib/math.hpp>
#include <emilib/tga.hpp>
#include <emilib/timer.hpp>
#include <loguru.hpp>

#include "sdf.hpp"

using namespace emilib::math;

struct RGBA
{
	uint8_t r, g, b, a;
};

struct Shape
{
	bool   inverted     = false;
	size_t num_points   = 64;

	float  center       =  0.5f;
	float  radius       =  0.35f;

	float  squareness   =  0;

	float  angle_offset =  0;
};

struct Options
{
	int                seed             =  0;
	size_t             resolution       = 16;
	std::vector<Shape> shapes;
	float              pos_noise        =  0.005f;
	float              dir_noise        =  0.05f;
	Strengths          strengths;
	bool               double_precision = true;

	Options()
	{
		shapes.push_back(Shape{});
		{
			Shape c;
			c.inverted = true;
			c.radius = 0.1;
			shapes.push_back(c);
		}
	}
};

struct Point
{
	float x, y, dx ,dy;
};

struct Result
{
	std::vector<Point> points;
	std::vector<float> sdf;
	std::vector<RGBA>  sdf_image;
	std::vector<RGBA>  blob_image;
	float              blob_area;
	double             duration_seconds;
};

void generate_points(std::vector<Point>* out_points, const Shape& shape)
{
	CHECK_NOTNULL_F(out_points);
	float sign = shape.inverted ? -1 : +1;

	using Dualf = emilib::Dual<float>;

	for (size_t i = 0; i < shape.num_points; ++i)
	{
		Dualf angle = sign * Dualf(i * float(M_PI) * 2 / shape.num_points, 1);
		Dualf square_rad_factor = Dualf(1.0f) / std::max(std::abs(std::cos(angle)), std::abs(std::sin(angle)));
		Dualf radius = shape.radius * lerp(Dualf(1), square_rad_factor, shape.squareness);

		angle.real += shape.angle_offset;

		Dualf x = shape.center + radius * std::cos(angle);
		Dualf y = shape.center + radius * std::sin(angle);

		float dx = x.eps;
		float dy = y.eps;
		float normal_norm = std::hypot(dx, dy);
		dx /= normal_norm;
		dy /= normal_norm;

		out_points->push_back(Point{x.real, y.real, dy, -dx});
	}
}

float area(const std::vector<Shape>& shapes)
{
	// TODO: calculate by oversampling + using calc_area in marching_cubes.hpp
	double expected_area = 0;
	for (const auto& shape : shapes) {
		float sign = shape.inverted ? -1 : +1;
		expected_area += sign * M_PI * std::pow(shape.radius, 2);
	}
	return expected_area;
}

std::vector<float> generate_sdf(const std::vector<Point>& points, const Options& options)
{
	const auto width = options.resolution;
	const auto height = options.resolution;

	LinearEquation eq;
	add_model_constraints(&eq, width, height, options.strengths);

	// Data constraints:
	for (const auto& point : points) {
		const float pos[2]    = {point.x,  point.y};
		const float normal[2] = {point.dx, point.dy};
		add_point_constraint(&eq, width, height, options.strengths, pos, normal);
	}

	LOG_F(INFO, "%lu equations", eq.rhs.size());
	LOG_F(INFO, "%lu values in matrix", eq.triplets.size());

	const size_t num_unknowns = width * height;
	auto sdf = solve_sparse_linear(num_unknowns, eq.triplets, eq.rhs, options.double_precision);
	if (sdf.size() != num_unknowns) {
		LOG_F(ERROR, "Failed to find a solution");
		sdf.resize(num_unknowns, 0.0f);
	}
	return sdf;
}

Result generate(const Options& options)
{
	ERROR_CONTEXT("resolution", options.resolution);

	emilib::Timer timer;
	std::default_random_engine rng(options.seed);
	const int resolution = options.resolution;

	Result result;

	for (const auto& shape : options.shapes) {
		generate_points(&result.points, shape);
	}

	std::normal_distribution<float> pos_noise(0.0, options.pos_noise);
	std::normal_distribution<float> dir_noise(0.0, options.dir_noise);

	std::vector<Point> points_on_lattice;
	points_on_lattice.reserve(result.points.size());

	for (auto& point : result.points) {
		point.x += pos_noise(rng);
		point.y += pos_noise(rng);
		float angle = std::atan2(point.dy, point.dx);
		angle += dir_noise(rng);
		point.dx += std::cos(angle);
		point.dy += std::sin(angle);

		Point on_lattice = point;
		on_lattice.x *= (resolution - 1.0f);
		on_lattice.y *= (resolution - 1.0f);
		points_on_lattice.push_back(on_lattice);
	}

	result.sdf = generate_sdf(points_on_lattice, options);

	double area_pixels = 0;

	float max_abs_dist = 1e-6f;
	for (const float dist : result.sdf) {
		max_abs_dist = std::max(max_abs_dist, std::abs(dist));
	}

	for (const float dist : result.sdf) {
		const uint8_t dist_u8 = std::min<float>(255, 255 * std::abs(dist) / max_abs_dist);
		const uint8_t inv_dist_u8 = 255 - dist_u8;;
		if (dist < 0) {
			result.sdf_image.emplace_back(RGBA{inv_dist_u8, inv_dist_u8, 255, 255});
		} else {
			result.sdf_image.emplace_back(RGBA{255, inv_dist_u8, inv_dist_u8, 255});
		}

		float insideness = 1 - std::max(0.0, std::min(1.0, (dist + 0.5) * 2));
		const uint8_t color = 255 * insideness;
		result.blob_image.emplace_back(RGBA{color, color, color, 255});

		area_pixels += insideness;
	}

	result.blob_area = area_pixels / sqr(resolution - 1);

	result.duration_seconds = timer.secs();
	return result;
}

bool showshapeOption(Shape* shape)
{
	bool changed = false;

	ImGui::Text("Shape:");
	changed |= ImGui::Checkbox("inverted (hole)", &shape->inverted);
	changed |= ImGuiPP::SliderSize("num_points",  &shape->num_points,    1, 1024, 2);
	changed |= ImGui::SliderFloat("center",       &shape->center,        0,    1);
	changed |= ImGui::SliderFloat("radius",       &shape->radius,        0,    1);
	changed |= ImGui::SliderFloat("squareness",   &shape->squareness,   -2,       3);
	changed |= ImGui::SliderAngle("angle_offset", &shape->angle_offset,  0,  360);

	return changed;
}

bool showStrengths(Strengths* strengths)
{
	bool changed = false;

	ImGui::Text("How much we trust the data:");
	changed |= ImGui::SliderFloat("data_pos",    &strengths->data_pos,    0, 10, "%.4f", 4);
	changed |= ImGui::SliderFloat("data_normal", &strengths->data_normal, 0, 10, "%.4f", 4);
	ImGui::Text("How much we trust the model:");
	changed |= ImGui::SliderFloat("model_0",     &strengths->model_0,     0, 10, "%.4f", 4);
	changed |= ImGui::SliderFloat("model_1",     &strengths->model_1,     0, 10, "%.4f", 4);
	changed |= ImGui::SliderFloat("model_2",     &strengths->model_2,     0, 10, "%.4f", 4);
	changed |= ImGui::SliderFloat("model_3",     &strengths->model_3,     0, 10, "%.4f", 4);

	return changed;
}

bool showOptions(Options* options)
{
	bool changed = false;

	if (ImGui::Button("Reset all")) {
		*options = {};
		changed = true;
	}
	changed |= ImGui::SliderInt("seed", &options->seed, 0, 100);
	changed |= ImGuiPP::SliderSize("resolution", &options->resolution, 4, 256);
	ImGui::Separator();
	for (const int i : emilib::indices(options->shapes)) {
		ImGui::PushID(i);
		changed |= showshapeOption(&options->shapes[i]);
		ImGui::PopID();
		ImGui::Separator();
	}
	if (options->shapes.size() >= 2 && ImGui::Button("Remove shape")) {
		options->shapes.pop_back();
		changed = true;
		ImGui::SameLine();
	}
	if (ImGui::Button("Add shape")) {
		options->shapes.push_back(Shape{});
		changed = true;
	}
	ImGui::Separator();
	changed |= ImGui::SliderFloat("pos_noise", &options->pos_noise, 0, 0.1, "%.4f");
	changed |= ImGui::SliderAngle("dir_noise", &options->dir_noise, 0,    360);
	ImGui::Separator();
	changed |= showStrengths(&options->strengths);
	changed |= ImGui::Checkbox("Solve with double precision", &options->double_precision);

	return changed;
}

ImGuiWindowFlags fullscreen_window_flags()
{
	ImGuiIO& io = ImGui::GetIO();
	const float width = io.DisplaySize.x;
	const float height = io.DisplaySize.y;
	ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiSetCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiSetCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints({width, height}, {width, height});
	return ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
}

void show_cells(const Options& options, ImVec2 canvas_pos, ImVec2 canvas_size)
{
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	// Draw "voxel" sides
	for (size_t i : emilib::irange<size_t>(0, options.resolution - 1)) {
		const float left   = canvas_pos.x;
		const float right  = canvas_pos.x + canvas_size.x;
		const float top    = canvas_pos.y;
		const float bottom = canvas_pos.y + canvas_size.y;
		const float center_f = static_cast<float>(i + 0.5f) / (options.resolution - 1.0f);
		const float center_x = canvas_pos.x + canvas_size.x * center_f;
		const float center_y = canvas_pos.y + canvas_size.y * center_f;
		draw_list->AddLine({left, center_y}, {right, center_y}, ImColor(1.0f, 1.0f, 1.0f, 0.25f));
		draw_list->AddLine({center_x, top}, {center_x, bottom}, ImColor(1.0f, 1.0f, 1.0f, 0.25f));
	}

	if (options.resolution < 64) {
		// Draw sample points
		for (size_t xi : emilib::irange<size_t>(0, options.resolution)) {
			for (size_t yi : emilib::irange<size_t>(0, options.resolution)) {
				const float x = static_cast<float>(xi) / (options.resolution - 1.0f);
				const float y = static_cast<float>(yi) / (options.resolution - 1.0f);
				const float center_x = canvas_pos.x + canvas_size.x * x;
				const float center_y = canvas_pos.y + canvas_size.y * y;
				draw_list->AddCircleFilled({center_x, center_y}, 1, ImColor(1.0f, 1.0f, 1.0f, 0.25f), 4);
			}
		}
	}
}

void show_points(const Options& options, const std::vector<Point>& points, ImVec2 canvas_pos, ImVec2 canvas_size)
{
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	for (const auto& point : points) {
		ImVec2 center;
		center.x = canvas_pos.x + canvas_size.x * point.x;
		center.y = canvas_pos.y + canvas_size.y * point.y;
		draw_list->AddCircleFilled(center, 1, ImColor(1.0f, 1.0f, 1.0f, 1.0f));
		const float arrow_len = 5;
		draw_list->AddLine(center, ImVec2{center.x + arrow_len * point.dx, center.y + arrow_len * point.dy}, ImColor(1.0f, 1.0f, 1.0f, 0.75f));
	}
}

void show_blob(size_t resolution, const std::vector<float>& lines, ImVec2 canvas_pos, ImVec2 canvas_size)
{
	ImDrawList* draw_list = ImGui::GetWindowDrawList();

	CHECK_F(lines.size() % 4 == 0);

	for (size_t i = 0; i < lines.size(); i += 4) {
		float x0 = lines[i + 0];
		float y0 = lines[i + 1];
		float x1 = lines[i + 2];
		float y1 = lines[i + 3];

		x0 = canvas_pos.x + canvas_size.x * (x0 / (resolution - 1.0f));
		y0 = canvas_pos.y + canvas_size.y * (y0 / (resolution - 1.0f));
		x1 = canvas_pos.x + canvas_size.x * (x1 / (resolution - 1.0f));
		y1 = canvas_pos.y + canvas_size.y * (y1 / (resolution - 1.0f));

		draw_list->AddLine({x0, y0}, {x1, y1}, ImColor(1.0f, 0.0f, 0.0f, 1.0f));
#if 1
		float cx = (x0 + x1) / 2;
		float cy = (y0 + y1) / 2;
		float dx = (x1 - x0);
		float dy = (y1 - y0);
		float norm = 10 / std::hypot(dx, dy);
		dx *= norm;
		dy *= norm;
		draw_list->AddLine({cx, cy}, {cx + dy, cy - dx}, ImColor(0.0f, 1.0f, 0.0f, 1.0f));
#endif
	}
}
int main(int argc, char* argv[])
{
	loguru::g_colorlogtostderr = false;
	loguru::init(argc, argv);

	emilib::sdl::Params sdl_params;
	sdl_params.window_name = "2D SDF generator";
	sdl_params.width_points = 1800;
	sdl_params.height_points = 1200;
	auto sdl = emilib::sdl::init(sdl_params);

	emilib::ImGui_SDL imgui_sdl(sdl.width_points, sdl.height_points, sdl.pixels_per_point);

	gl::bind_imgui_painting();

	Options options;
	auto result = generate(options);

	gl::Texture sdf_texture{"sdf", gl::TexParams::clamped_nearest()};
	gl::Texture blob_texture{"blob", gl::TexParams::clamped_nearest()};

	const auto image_size = gl::Size{static_cast<unsigned>(options.resolution), static_cast<unsigned>(options.resolution)};
	sdf_texture.set_data(result.sdf_image.data(),   image_size, gl::ImageFormat::RGBA32);
	blob_texture.set_data(result.blob_image.data(), image_size, gl::ImageFormat::RGBA32);

	bool draw_points = true;
	bool draw_blob = true;

	bool quit = false;
	while (!quit) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_QUIT) { quit = true; }
			imgui_sdl.on_event(event);
		}
		imgui_sdl.new_frame();

		ImGui::ShowTestWindow();

		if (ImGui::Begin("Input")) {
			if (showOptions(&options)) {
				result = generate(options);
				const auto image_size = gl::Size{static_cast<unsigned>(options.resolution), static_cast<unsigned>(options.resolution)};
				sdf_texture.set_data(result.sdf_image.data(),   image_size, gl::ImageFormat::RGBA32);
				blob_texture.set_data(result.blob_image.data(), image_size, gl::ImageFormat::RGBA32);
			}
		}

		if (ImGui::Begin("Result")) {
			const auto lines = emilib::marching_squares(options.resolution, options.resolution, result.sdf.data());
			const float lines_area = emilib::calc_area(lines.size() / 4, lines.data()) / sqr(options.resolution - 1);

			ImGui::Text("Calculated in %.3f s", result.duration_seconds);
			ImGui::Text("Model area: %.3f, marching squares area: %.3f, sdf blob area: %.3f",
				area(options.shapes), lines_area, result.blob_area);

			ImGui::Checkbox("Input points", &draw_points);
			ImGui::SameLine();
			ImGui::Checkbox("Output blob", &draw_blob);

			ImVec2 canvas_size{384, 384};
			ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
			ImGui::InvisibleButton("canvas", canvas_size);
			show_cells(options, canvas_pos, canvas_size);
			if (draw_points) { show_points(options,          result.points, canvas_pos, canvas_size); }
			if (draw_blob)   { show_blob(options.resolution, lines,         canvas_pos, canvas_size); }

			ImGui::Image(reinterpret_cast<ImTextureID>(sdf_texture.id()), canvas_size);
			ImGui::SameLine();
			ImGui::Image(reinterpret_cast<ImTextureID>(blob_texture.id()), canvas_size);

			if (ImGui::Button("Save images")) {
				const auto res = options.resolution;
				const bool alpha = false;
				CHECK_F(emilib::write_tga("sdf.tga",  res, res, result.sdf_image.data(),  alpha));
				CHECK_F(emilib::write_tga("blob.tga", res, res, result.blob_image.data(), alpha));
			}
		}

		glClearColor(0.1f, 0.1f, 0.1f, 0);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		imgui_sdl.paint();

		SDL_GL_SwapWindow(sdl.window);
	}
}
