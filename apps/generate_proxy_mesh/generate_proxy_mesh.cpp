#include <iostream>
#include <cassert>
#include <algorithm>

#include <fmt/format.h>

#include "util/system.h"
#include "util/arguments.h"

#include "mve/mesh_io_ply.h"
#include "mve/image_io.h"

#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"

#include "acc/kd_tree.h"

constexpr float lowest = std::numeric_limits<float>::lowest();

struct AABB {
    math::Vec3f min;
    math::Vec3f max;
};

struct Arguments {
    std::string cloud;
    std::string mesh;
    std::string hmap;
    bool fuse;
    float resolution;
};

Arguments parse_args(int argc, char **argv) {
    util::Arguments args;
    args.set_exit_on_error(true);
    args.set_nonopt_minnum(2);
    args.set_nonopt_maxnum(2);
    args.set_usage("Usage: " + std::string(argv[0]) + " [OPTS] CLOUD OUT_MESH");
    args.set_description("TODO");
    args.add_option('r', "resolution", true, "height map resolution [1.0]");
    args.add_option('h', "height-map", true, "save height map as pfm file");
    args.add_option('f', "fuse-samples", false, "save height map as pfm file");
    args.parse(argc, argv);

    Arguments conf;
    conf.cloud = args.get_nth_nonopt(0);
    conf.mesh = args.get_nth_nonopt(1);
    conf.resolution = 1.0f;
    conf.fuse = false;

    for (util::ArgResult const* i = args.next_option();
         i != 0; i = args.next_option()) {
        switch (i->opt->sopt) {
        case 'r':
            conf.resolution = i->get_arg<float>();
        break;
        case 'h':
            conf.hmap = i->arg;
        break;
        case 'f':
            conf.fuse = true;
        break;
        default:
            throw std::invalid_argument("Invalid option");
        }
    }

    return conf;
}

inline
float volume(AABB const & aabb) {
    math::Vec3f diff = aabb.max - aabb.min;
    for (int i = 0; i < 3; ++i) {
        if (diff[i] <= 0.0f) return 0.0f;
    }
    return diff[0] * diff[1] * diff[2];
}

inline
void
patch(mve::FloatImage::Ptr img, int x, int y, float (*ptr)[3][3]) {
    for (int i = -1; i <= 1; ++i) {
        for (int j = -1; j <= 1; ++j) {
            (*ptr)[1 + j][1 + i] = img->at(x + j, y + i, 0);
        }
    }
}

int main(int argc, char **argv) {
    util::system::register_segfault_handler();
    util::system::print_build_timestamp(argv[0]);

    Arguments args = parse_args(argc, argv);

    mve::TriangleMesh::Ptr cloud;
    try {
        cloud = mve::geom::load_ply_mesh(args.cloud);
    } catch (std::exception& e) {
        std::cerr << "\tCould not load cloud: "<< e.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }

    assert(cloud->get_faces().empty());

    cloud->get_vertex_colors().clear();
    std::vector<math::Vec3f> const & verts = cloud->get_vertices();
    std::vector<math::Vec3f> const & normals = cloud->get_vertex_normals();
    std::vector<float> const & values = cloud->get_vertex_values();
    std::vector<float> const & confidences = cloud->get_vertex_confidences();

    AABB aabb;
    aabb.min = math::Vec3f(std::numeric_limits<float>::max());
    aabb.max = math::Vec3f(-std::numeric_limits<float>::max());
    for (std::size_t i = 0; i < verts.size(); ++i) {
        for (int j = 0; j < 3; ++j) {
            aabb.min[j] = std::min(aabb.min[j], verts[i][j]);
            aabb.max[j] = std::max(aabb.max[j], verts[i][j]);
        }
    }

    assert(volume(aabb) > 0.0f);

    int width = (aabb.max[0] - aabb.min[0]) / args.resolution + 1.0f;
    int height = (aabb.max[1] - aabb.min[1]) / args.resolution + 1.0f;

    std::cout << fmt::format("Creating height map ({}x{})", width, height) << std::endl;

    /* Create height map. */
    mve::FloatImage::Ptr hmap = mve::FloatImage::create(width, height, 1);
    hmap->fill(lowest);
    for (std::size_t i = 0; i < verts.size(); ++i) {
        math::Vec3f vertex = verts[i];
        /* ... + center offset + rounding */
        int x = (vertex[0] - aabb.min[0]) / args.resolution + args.resolution / 2.0f + 0.5f;
        int y = (vertex[1] - aabb.min[1]) / args.resolution + args.resolution / 2.0f + 0.5f;
        float height = vertex[2];
        float z = hmap->at(x, y, 0);
        if (z > height) continue;

        hmap->at(x, y, 0) = height;
    }

    /* Use median filter to eliminate outliers. */
    {
        mve::FloatImage::Ptr tmp = mve::FloatImage::create(width, height, 1);

        #pragma omp parallel for
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (y == 0 || y == height - 1 || x == 0 || x == width - 1) {
                    tmp->at(x, y, 0) = lowest;
                } else {
                    float heights[9];
                    patch(hmap, x, y, (float (*)[3][3])&heights);
                    std::sort(heights, heights + 9);
                    tmp->at(x, y, 0) = heights[4];
                }
            }
        }

        hmap.swap(tmp);
    }

    /* Fill holes within the height map. */
    bool holes = true;
    while (holes) {
        holes = false;

        mve::FloatImage::Ptr tmp = mve::FloatImage::create(width, height, 1);

        #pragma omp parallel for schedule(dynamic)
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (y == 0 || y == height - 1 || x == 0 || x == width - 1) {
                    tmp->at(x, y, 0) = lowest;
                } else if (hmap->at(x, y, 0) != lowest) {
                    tmp->at(x, y, 0) = hmap->at(x, y, 0);
                } else {
                    float heights[9];
                    patch(hmap, x, y, (float (*)[3][3])&heights);

                    float * end = std::remove(heights, heights + 9, lowest);

                    int n = std::distance(heights, end);

                    if (n >= 3) {
                        std::sort(heights, end);
                        tmp->at(x, y, 0) = heights[n / 2];
                    } else {
                        tmp->at(x, y, 0) = lowest;
                        holes = true;
                    }
                }
            }
        }

        hmap.swap(tmp);
    }

    /* Estimate ground level and normalize height map */
    float ground_level = std::numeric_limits<float>::max();
    #pragma omp parallel for reduction(min:ground_level)
    for (std::size_t i = 0; i < hmap->get_value_amount(); ++i) {
        float height = hmap->at(i);
        if (height != lowest && height < ground_level) {
            ground_level = height;
        }
    }

    #pragma omp parallel for
    for (std::size_t i = 0; i < hmap->get_value_amount(); ++i) {
        float height = hmap->at(i);
        if (height != lowest) {
            hmap->at(i) -= ground_level;
        } else {
            hmap->at(i) = 0.0f;
        }
    }

    if (!args.hmap.empty()) {
        mve::image::save_pfm_file(hmap, args.hmap);
    }

    acc::KDTree<3, unsigned> kd_tree(verts);
    fssr::IsoOctree octree;

    mve::TriangleMesh::Ptr scloud = mve::TriangleMesh::create();
    std::vector<math::Vec3f> & sverts = scloud->get_vertices();
    std::vector<math::Vec3f> & snormals = scloud->get_vertex_normals();

    /* Introduce artificial samples at height discontinuities. */
    #pragma omp parallel for schedule(dynamic)
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (y <= 1 || y >= height - 2 || x <= 1 || x >= width - 2) continue;

            float heights[3][3];
            patch(hmap, x, y, &heights);

            float rdx = -heights[0][1] + heights[1][1];
            float rdy = -heights[1][0] + heights[1][1];
            float fdx = -heights[1][1] + heights[2][1];
            float fdy = -heights[1][1] + heights[1][2];

            /* Calculate relevant magnitude. */
            float m = std::max(std::max(rdx, -fdx), std::max(rdy, -fdy));

            float gx =
                -heights[0][0] + heights[2][0]
                +2 * (-heights[0][1] + heights[2][1])
                -heights[0][2] + heights[2][2];
            float gy =
                -heights[0][0] + heights[0][2]
                +2 * (-heights[1][0] + heights[1][2])
                -heights[2][0] + heights[2][2];

            math::Vec3f normal(-gx, -gy, 0.0f); normal.normalize();
            float px = (x - args.resolution / 2.0f) * args.resolution + aabb.min[0];
            float py = (y - args.resolution / 2.0f) * args.resolution + aabb.min[1];

            if (m <= args.resolution) {
                if (!args.fuse) {
                    #pragma omp critical
                    {
                        sverts.emplace_back(px, py, heights[1][1] + ground_level);
                        snormals.emplace_back(0.0f, 0.0f, 1.0f);
                    }
                }
                continue;
            }

            #pragma omp critical
            {
                sverts.emplace_back(px, py, heights[1][1] + ground_level);
                snormals.push_back((math::Vec3f(0.0f, 0.0f, 1.0f) + normal).normalize());
            }

            for (int i = 1; i <= m / args.resolution; ++i) {
                float pz = ground_level + heights[1][1] - i * args.resolution;
                math::Vec3f vertex(px, py, pz);

                if (args.fuse && kd_tree.find_nn(vertex, nullptr, args.resolution)) continue;

                if (fdx > 0.0f && rdx < 0.0f && fdy > 0.0f && rdy < 0.0f) continue;

                #pragma omp critical
                {
                    sverts.push_back(vertex);
                    snormals.push_back(normal);
                }
            }
        }
    }

    /* Let's make some space. */
    hmap.reset();

    #if 1
    {
        mve::geom::SavePLYOptions opts;
        opts.write_vertex_normals = true;
        mve::geom::save_ply_mesh(scloud, "/tmp/test.ply", opts);
    }
    #endif

    for (std::size_t i = 0; i < sverts.size(); ++i) {
        fssr::Sample sample;
        sample.pos = sverts[i];
        sample.normal = snormals[i];
        sample.scale = args.resolution;
        sample.confidence = 0.5f;
        sample.color = math::Vec3f(0.0f, 0.0f, 1.0f);
        octree.insert_sample(sample);
    }

    if (args.fuse) {
        for (std::size_t i = 0; i < verts.size(); ++i) {
            fssr::Sample sample;
            sample.pos = verts[i];
            sample.normal = normals[i];
            sample.scale = values[i];
            sample.confidence = confidences[i];
            sample.color = math::Vec3f(0.7f);
            octree.insert_sample(sample);
        }
    }

    /* Perform fssrecon c.f. mve/apps/fssrecon/fssrecon.cc */
    octree.limit_octree_level();
    octree.compute_voxels();
    octree.clear_samples();
    fssr::IsoSurface iso_surface(&octree, fssr::INTERPOLATION_CUBIC);
    mve::TriangleMesh::Ptr mesh = iso_surface.extract_mesh();

    {
        std::size_t num_vertices = mesh->get_vertices().size();
        std::vector<float> confidences = mesh->get_vertex_confidences();
        mve::TriangleMesh::DeleteList delete_verts(num_vertices, false);
        for (std::size_t i = 0; i < num_vertices; ++i) {
            if (confidences[i] == 0.0f) {
                delete_verts[i] = true;
            }
        }
        mesh->delete_vertices_fix_faces(delete_verts);
    }

    mve::geom::SavePLYOptions opts;
    opts.write_vertex_normals = true;
    mve::geom::save_ply_mesh(mesh, args.mesh, opts);

    return EXIT_SUCCESS;
}