/**************************************************************************/
/*  test_environment.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_environment)

#ifndef _3D_DISABLED

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "scene/resources/environment.h"
#include "servers/rendering/rendering_server.h"
#include "tests/test_utils.h"

namespace TestEnvironment {

TEST_CASE("[MetalRT] path-tracing denoiser defaults and values are preserved") {
	Ref<Environment> environment;
	environment.instantiate();
	CHECK(environment->get_pathtracing_denoiser() == RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION);

	bool default_is_valid = false;
	const Variant default_value = ClassDB::class_get_default_property_value("Environment", "pathtracing_denoiser", &default_is_valid);
	CHECK(default_is_valid);
	CHECK(int(default_value) == int(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION));

	environment->set_pathtracing_denoiser(RSE::PT_DENOISER_METALFX);
	CHECK(environment->get_pathtracing_denoiser() == RSE::PT_DENOISER_METALFX);
}

TEST_CASE("[MetalRT] PRESENTATION serializes the selected denoiser") {
	Ref<Environment> environment;
	environment.instantiate();
	environment->set_pathtracing_enabled(true);
	environment->set_pathtracing_denoiser(RSE::PT_DENOISER_METALFX);

	const String save_path = TestUtils::get_temp_path("presentation_environment.tres");
	REQUIRE(ResourceSaver::save(environment, save_path) == OK);
	Ref<Environment> loaded = ResourceLoader::load(save_path, "Environment", ResourceFormatLoader::CACHE_MODE_IGNORE);
	REQUIRE(loaded.is_valid());
	CHECK(loaded->get_pathtracing_denoiser() == RSE::PT_DENOISER_METALFX);
}

TEST_CASE("[MetalRT] RenderingServer reports path-tracing denoiser capabilities") {
	RenderingServer *rendering_server = RenderingServer::get_singleton();
	REQUIRE(rendering_server != nullptr);
	CHECK(rendering_server->is_pathtracing_denoiser_supported(RSE::PT_DENOISER_NONE));
	// Extra parentheses: doctest cannot decompose a `||` expression.
	CHECK((rendering_server->is_pathtracing_denoiser_supported(RSE::PT_DENOISER_DLSS_RAY_RECONSTRUCTION) ||
			rendering_server->get_current_rendering_driver_name() != "metal"));
}

} // namespace TestEnvironment

#endif // _3D_DISABLED
