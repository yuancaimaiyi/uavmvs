/*
 * Copyright (C) 2016-2018, Nils Moehrle
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD 3-Clause license. See the LICENSE.txt file for details.
 */

#include "trajectory_io.h"

UTP_NAMESPACE_BEGIN

void save_trajectory(Trajectory const & trajectory,
    std::string const & path)
{
    std::ofstream out(path.c_str());

    if (!out.good()) throw std::runtime_error("Could not open trajectory file for writing");

    std::size_t length = trajectory.size();
    out << length << std::endl;

    for (std::size_t i = 0; i < length; ++i) {
        mve::CameraInfo const & cam = trajectory[i];
        math::Vec3f trans(cam.trans);
        math::Matrix3f rot(cam.rot);
        math::Vec3f pos = -rot.transposed() * trans;

        out << pos << std::endl;
        out << rot;
        out << cam.flen << std::endl;
    }

    out.close();
}

void load_trajectory(std::string const & path,
    Trajectory * trajectory)
{
    std::ifstream in(path.c_str());

    if (!in.good()) throw std::runtime_error("Could not open trajectory file");

    std::size_t length;
    in >> length;

    trajectory->resize(length);

    for (std::size_t i = 0; i < length; ++i) {
        mve::CameraInfo & cam = trajectory->at(i);

        math::Vec3f pos;
        for (int j = 0; j < 3; ++j) {
            in >> pos[j];
        }
        math::Matrix3f rot;
        for (int j = 0; j < 9; ++j) {
            in >> rot[j];
        }
        math::Vec3f trans = -rot * pos;
        std::copy(trans.begin(), trans.end(), cam.trans);
        std::copy(rot.begin(), rot.end(), cam.rot);

        in >> cam.flen;
    }

    if (in.fail()) {
        in.close();
        throw std::runtime_error("Invalid trajectory file");
    }

    in.close();
}

UTP_NAMESPACE_END
