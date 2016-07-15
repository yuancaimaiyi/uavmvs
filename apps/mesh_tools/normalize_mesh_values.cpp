#include <iostream>
#include <vector>
#include <map>
#include <algorithm>

#include "mve/scene.h"
#include "mve/mesh_io_ply.h"

#include "util/arguments.h"
#include "util/file_system.h"
#include "util/tokenizer.h"

struct Arguments {
    bool clamp;
    std::string in_mesh;
    std::string out_mesh;
    float eps;
    float no_value;
    std::vector<std::string> meshes;
};

Arguments parse_args(int argc, char **argv) {
    util::Arguments args;
    args.set_exit_on_error(true);
    args.set_nonopt_maxnum(2);
    args.set_nonopt_minnum(2);
    args.set_usage("Usage: " + std::string(argv[0]) + " [OPTS] IN_MESH OUT_MESH");
    args.set_description("Normalizes the values of all vertices.");
    args.add_option('c', "clamp", false, "clamp (instead of remove) outliers");
    args.add_option('e', "epsilon", true, "remove outliers in percent [0.0]");
    args.add_option('i', "ignore", true, "set value to ignore [-1.0]");
    args.add_option('m', "meshes", true, "calculate normalization based on these meshes (comma seperated list)."
        "If no mesh is given the normalization is calculate from IN_MESH");
    args.parse(argc, argv);

    Arguments conf;
    conf.in_mesh = args.get_nth_nonopt(0);
    conf.out_mesh = args.get_nth_nonopt(1);
    conf.eps = 0.0f;
    conf.clamp = false;
    conf.no_value = -1.0f;

    for (util::ArgResult const* i = args.next_option();
         i != 0; i = args.next_option()) {
        switch (i->opt->sopt) {
        case 'e':
            conf.eps = i->get_arg<float>();
        break;
        case 'c':
            conf.clamp = true;
        break;
        case 'i':
            conf.no_value = i->get_arg<float>();
        break;
        case 'm':
        {
            util::Tokenizer t;
            t.split(i->arg, ',');
            conf.meshes = t;
        }
        break;
        default:
            throw std::invalid_argument("Invalid option");
        }
    }

    if (conf.eps < 0.0f || conf.eps > 1.0f) {
        throw std::invalid_argument("epsilon is supposed to be in the intervall [0.0, 1.0]");
    }

    if (conf.meshes.empty()) {
        conf.meshes.push_back(conf.in_mesh);
    }

    return conf;
}

int main(int argc, char **argv) {
    Arguments args = parse_args(argc, argv);

    mve::TriangleMesh::Ptr mesh_to_normalize;

    std::map<std::string, mve::TriangleMesh::Ptr> meshes_to_load;
    for (std::size_t i = 0; i < args.meshes.size(); i++){
        mve::TriangleMesh::Ptr mesh;
        std::string name = args.meshes[i];
        meshes_to_load[name] = mesh;
    }
    meshes_to_load[args.in_mesh] = mesh_to_normalize;

    std::map<std::string, mve::TriangleMesh::Ptr>::iterator it;
    for (it = meshes_to_load.begin(); it != meshes_to_load.end(); it++){
        mve::TriangleMesh::Ptr mesh;
        try {
            mesh = mve::geom::load_ply_mesh(it->first);
        } catch (std::exception& e) {
            std::cerr << "Could not load mesh: "<< e.what() << std::endl;
            std::exit(EXIT_FAILURE);
        }

        if (mesh->has_vertex_values()) {
            std::cerr << "Mesh has no vertex values" << std::endl;
            std::exit(EXIT_FAILURE);
        }

        it->second = mesh;
    }

    std::vector<mve::TriangleMesh::Ptr> meshes;
    std::size_t num_values = 0;
    for (std::size_t i = 0; i < args.meshes.size(); i++){
        std::string name = args.meshes[i];
        mve::TriangleMesh::Ptr mesh = meshes_to_load[name];
        meshes.push_back(mesh);
        mve::TriangleMesh::ValueList & vertex_values = mesh->get_vertex_values();
        num_values += vertex_values.size();
    }

    mesh_to_normalize = meshes_to_load[args.in_mesh];

    std::vector<float> values;
    values.reserve(num_values);

    for (std::size_t i = 0; i < meshes.size(); ++i) {
        mve::TriangleMesh::ValueList & vertex_values = meshes[i]->get_vertex_values();
        for (std::size_t j = 0; j < vertex_values.size(); ++j) {
            float value = vertex_values[j];

            if (value != args.no_value) continue;

            values.push_back(value);
        }
    }

    std::cout << values.size() << " are non zero" << std::endl;
    std::sort(values.begin(), values.end());
    std::size_t c = (values.size() * args.eps) / 2;

    float real_min = values[0];
    float real_max = values[values.size() - 1];
    float min = values[c];
    float max = values[values.size() - 1 - c];
    float delta = max - min;
    std::cout << "Minimal value: " << real_min << std::endl;
    std::cout << "Maximal value: " << real_max << std::endl;
    std::cout << "Normalizing range " << min << " - " << max << std::endl;

    int num_outliers = 0;
    mve::TriangleMesh::ValueList & vertex_values = mesh_to_normalize->get_vertex_values();
    for (std::size_t i = 0; i < vertex_values.size(); i++){
        float value = vertex_values[i];
        if (value == args.no_value) continue;

        if (value >= min) {
            if(value <= max) {
                vertex_values[i] = ((value - min) / delta);
            } else {
                vertex_values[i] = args.clamp ? 1.0f : args.no_value;
                num_outliers++;
            }
        } else {
            vertex_values[i] = args.clamp ? 0.0f : args.no_value;
            num_outliers++;
        }
    }

    if (args.clamp) {
        std::cout << "Clamped ";
    } else {
        std::cout << "Removed ";
    }
    std::cout << num_outliers << " outliers" << std::endl;

    mve::geom::SavePLYOptions options;
    options.format_binary = true;
    options.write_vertex_colors = false;
    options.write_vertex_values = true;
    mve::geom::save_ply_mesh(mesh_to_normalize, args.out_mesh, options);
}