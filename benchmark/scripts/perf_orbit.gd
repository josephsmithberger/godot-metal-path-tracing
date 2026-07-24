extends Node3D

# Orbits the camera continuously so every frame is a "camera moved, nothing else
# changed" frame -- the exact case the editor hits when the user drags to orbit.
#
# Run with GODOT_GPU_PROFILE=1 to get the per-pass GPU breakdown printed once a
# second by RenderingServerDefault.

const ORBIT_RADIUS := 9.0
const ORBIT_HEIGHT := 4.2
const ORBIT_SPEED := 0.6 # radians/sec
const WARMUP_FRAMES := 90
const MEASURE_FRAMES := 400

@onready var camera: Camera3D = $Camera

var _frame := 0
var _angle := 0.0
var _cpu_times: Array[float] = []
var _wall_frame_times: Array[float] = []
var _measure_start_usec := 0
var _orbit_enabled := true
var _mixed_alpha_enabled := false


func _ready() -> void:
	# Vsync caps the cheap configs at the 120Hz refresh, which lets the GPU idle
	# and downclock -- that inflates their measured pass time and makes the
	# ablations look closer together than they are.
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	_build_scene()
	if OS.get_environment("GODOT_PERF_NO_ORBIT") == "1":
		_orbit_enabled = false
	_apply_env_overrides()
	_place_camera()


func _apply_env_overrides() -> void:
	var environment: Environment = $WorldEnvironment.environment
	var samples := OS.get_environment("GODOT_PERF_SPP")
	if samples != "":
		environment.pathtracing_samples_per_pixel = int(samples)
	var bounces := OS.get_environment("GODOT_PERF_BOUNCES")
	if bounces != "":
		environment.pathtracing_max_bounces = int(bounces)
	var denoiser := OS.get_environment("GODOT_PERF_DENOISER")
	if denoiser != "":
		environment.pathtracing_denoiser = int(denoiser)

	var scale := OS.get_environment("GODOT_PERF_SCALE")
	if scale != "":
		get_viewport().scaling_3d_scale = float(scale)

	# Debug vis modes break out of the path loop at known points, which turns them
	# into free ablations: 23 (INSTANCE_ID) stops right after the primary trace and
	# hit-data fetch, before any material eval, NEE, or bounce.
	var debug_mode := OS.get_environment("GODOT_PERF_DEBUG_MODE")
	if debug_mode != "":
		environment.pathtracing_debug_mode = int(debug_mode)

	# light_count == 0 makes the shader skip direct lighting entirely, which
	# ablates the NEE shadow ray without touching the shader.
	if OS.get_environment("GODOT_PERF_NO_LIGHT") == "1":
		$KeyLight.visible = false

	print("PERF_ORBIT_CONFIG spp=%d bounces=%d denoiser=%d scale=%.2f" % [
		environment.pathtracing_samples_per_pixel,
		environment.pathtracing_max_bounces,
		environment.pathtracing_denoiser,
		get_viewport().scaling_3d_scale,
	])


func _build_scene() -> void:
	# A handful of distinct meshes/materials so the TLAS has real membership and
	# the material dispatch path is exercised, without making BLAS build the
	# dominant cost.
	var rng := RandomNumberGenerator.new()
	rng.seed = 20260715

	var floor_mesh := BoxMesh.new()
	floor_mesh.size = Vector3(24.0, 0.5, 24.0)
	var floor_instance := MeshInstance3D.new()
	floor_instance.mesh = floor_mesh
	floor_instance.position = Vector3(0.0, -0.25, 0.0)
	var floor_material := StandardMaterial3D.new()
	floor_material.albedo_color = Color(0.55, 0.55, 0.58)
	floor_material.roughness = 0.7
	floor_instance.material_override = floor_material
	add_child(floor_instance)

	for i in range(24):
		var instance := MeshInstance3D.new()
		if i % 3 == 0:
			var sphere := SphereMesh.new()
			sphere.radius = 0.6
			sphere.height = 1.2
			instance.mesh = sphere
		elif i % 3 == 1:
			var box := BoxMesh.new()
			box.size = Vector3(1.0, 1.0, 1.0)
			instance.mesh = box
		else:
			var cylinder := CylinderMesh.new()
			cylinder.top_radius = 0.45
			cylinder.bottom_radius = 0.45
			cylinder.height = 1.4
			instance.mesh = cylinder

		var material := StandardMaterial3D.new()
		material.albedo_color = Color(rng.randf_range(0.2, 0.9), rng.randf_range(0.2, 0.9), rng.randf_range(0.2, 0.9))
		material.roughness = rng.randf_range(0.05, 0.9)
		material.metallic = 1.0 if i % 4 == 0 else 0.0
		instance.material_override = material

		var ring_angle := TAU * float(i) / 24.0
		var ring_radius := 2.0 + 2.4 * float(i % 3)
		instance.position = Vector3(cos(ring_angle) * ring_radius, 0.7 + 0.5 * float(i % 2), sin(ring_angle) * ring_radius)
		add_child(instance)

	if OS.get_environment("GODOT_PERF_MIXED_ALPHA") == "1":
		_mixed_alpha_enabled = true
		_build_mixed_alpha_cards()


func _build_mixed_alpha_cards() -> void:
	# Dense checker-cut cards force rays to reject several alpha candidates
	# before reaching opaque geometry. This intentionally stresses candidate
	# evaluation rather than merely adding one decorative transparent surface.
	var alpha_image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in range(64):
		for x in range(64):
			var cell_x := x >> 2
			var cell_y := y >> 2
			var alpha := 1.0 if (cell_x + cell_y) % 2 == 0 else 0.0
			alpha_image.set_pixel(x, y, Color(0.55, 0.8, 0.35, alpha))
	var alpha_texture := ImageTexture.create_from_image(alpha_image)

	for i in range(48):
		var card_mesh := QuadMesh.new()
		card_mesh.size = Vector2(2.2, 2.0)
		var card := MeshInstance3D.new()
		card.mesh = card_mesh
		var material := StandardMaterial3D.new()
		material.albedo_texture = alpha_texture
		material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
		material.alpha_scissor_threshold = 0.5
		material.cull_mode = BaseMaterial3D.CULL_DISABLED
		material.roughness = 0.8
		card.material_override = material

		var ring_angle := TAU * float(i) / 48.0
		var ring_radius := 1.4 + 0.65 * float(i % 5)
		card.position = Vector3(cos(ring_angle) * ring_radius, 1.0 + 0.3 * float(i % 3), sin(ring_angle) * ring_radius)
		card.rotation.y = -ring_angle + PI * 0.5
		add_child(card)


func _place_camera() -> void:
	var origin := Vector3(cos(_angle) * ORBIT_RADIUS, ORBIT_HEIGHT, sin(_angle) * ORBIT_RADIUS)
	camera.look_at_from_position(origin, Vector3(0.0, 0.7, 0.0))


func _process(delta: float) -> void:
	if _orbit_enabled:
		_angle += ORBIT_SPEED * delta
		_place_camera()

	_frame += 1
	if _frame == WARMUP_FRAMES:
		_measure_start_usec = Time.get_ticks_usec()
	if _frame > WARMUP_FRAMES:
		_cpu_times.append(Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0)
		_wall_frame_times.append(delta * 1000.0)

	var measure_frames := MEASURE_FRAMES
	var measure_env := OS.get_environment("GODOT_PERF_MEASURE_FRAMES")
	if measure_env != "":
		measure_frames = int(measure_env)
	if _frame >= WARMUP_FRAMES + measure_frames:
		_report()
		# Optional: set GODOT_PERF_SCREENSHOT to a file path to capture the final
		# measured frame (useful for verifying the render on headless/remote runs).
		var _shot_path := OS.get_environment("GODOT_PERF_SCREENSHOT")
		if _shot_path != "":
			var _img := get_viewport().get_texture().get_image()
			if _img != null:
				_img.save_png(_shot_path)
				print("PERF_ORBIT_SCREENSHOT=%s" % _shot_path)
		get_tree().quit()


func _report() -> void:
	var elapsed_seconds := float(Time.get_ticks_usec() - _measure_start_usec) / 1000000.0
	var average_fps := float(_wall_frame_times.size()) / elapsed_seconds
	print("PERF_ORBIT_FRAMES=%d" % _frame)
	print("PERF_ORBIT_MEASURED_FRAMES=%d" % _wall_frame_times.size())
	print("PERF_ORBIT_AVG_FPS=%.2f" % average_fps)
	print("PERF_ORBIT_FRAME_TIME_MS_AVG=%.3f" % _average(_wall_frame_times))
	print("PERF_ORBIT_FRAME_TIME_MS_P50=%.3f" % _percentile(_wall_frame_times, 0.50))
	print("PERF_ORBIT_FRAME_TIME_MS_P95=%.3f" % _percentile(_wall_frame_times, 0.95))
	print("PERF_ORBIT_FRAME_TIME_MS_P99=%.3f" % _percentile(_wall_frame_times, 0.99))
	print("PERF_ORBIT_FRAME_TIME_MS_MAX=%.3f" % _percentile(_wall_frame_times, 1.00))
	print("PERF_ORBIT_CPU_TIME_MS_AVG=%.3f" % _average(_cpu_times))
	print("PERF_ORBIT_CPU_TIME_MS_P95=%.3f" % _percentile(_cpu_times, 0.95))
	print("PERF_ORBIT_VIDEO_ADAPTER=%s" % RenderingServer.get_video_adapter_name())
	print("PERF_ORBIT_VIDEO_VENDOR=%s" % RenderingServer.get_video_adapter_vendor())
	print("PERF_ORBIT_RENDERING_DRIVER=%s" % RenderingServer.get_current_rendering_driver_name())
	print("PERF_ORBIT_RENDERING_METHOD=%s" % RenderingServer.get_current_rendering_method())
	print("PERF_ORBIT_ORBIT_ENABLED=%s" % str(_orbit_enabled))
	print("PERF_ORBIT_MIXED_ALPHA=%s" % str(_mixed_alpha_enabled))


func _average(values: Array[float]) -> float:
	if values.is_empty():
		return 0.0
	var total := 0.0
	for value in values:
		total += value
	return total / float(values.size())


func _percentile(values: Array[float], fraction: float) -> float:
	if values.is_empty():
		return 0.0
	var sorted := values.duplicate()
	sorted.sort()
	var index := int(round(float(sorted.size() - 1) * fraction))
	return sorted[index]
