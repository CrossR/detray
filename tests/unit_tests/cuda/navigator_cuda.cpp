/** Detray library, part of the ACTS project (R&D line)
 *
 * (c) 2022 CERN for the benefit of the ACTS project
 *
 * Mozilla Public License Version 2.0
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <vecmem/memory/cuda/device_memory_resource.hpp>
#include <vecmem/memory/cuda/managed_memory_resource.hpp>

#include "navigator_cuda_kernel.hpp"
#include "vecmem/utils/cuda/copy.hpp"

TEST(navigator_cuda, navigator) {

    // Helper object for performing memory copies.
    vecmem::cuda::copy copy;

    // VecMem memory resource(s)
    vecmem::cuda::managed_memory_resource mng_mr;
    vecmem::cuda::device_memory_resource dev_mr;

    // Create detector
    detector_host_t det =
        create_toy_geometry<darray, thrust::tuple, vecmem::vector,
                            vecmem::jagged_vector>(mng_mr, n_brl_layers,
                                                   n_edc_layers);

    // Create navigator
    navigator_host_t n(det);

    // Create the vector of initial track parameters
    vecmem::vector<free_track_parameters> tracks_host(&mng_mr);
    vecmem::vector<free_track_parameters> tracks_device(&mng_mr);

    // Set origin position of tracks
    const point3 ori{0., 0., 0.};

    // Loops of theta values ]0,pi[
    for (unsigned int itheta = 0; itheta < theta_steps; ++itheta) {
        scalar theta = 0.001 + itheta * (M_PI - 0.001) / theta_steps;
        scalar sin_theta = std::sin(theta);
        scalar cos_theta = std::cos(theta);

        // Loops of phi values [-pi, pi]
        for (unsigned int iphi = 0; iphi < phi_steps; ++iphi) {
            // The direction
            scalar phi = -M_PI + iphi * (2 * M_PI) / phi_steps;
            scalar sin_phi = std::sin(phi);
            scalar cos_phi = std::cos(phi);
            vector3 dir{cos_phi * sin_theta, sin_phi * sin_theta, cos_theta};

            // intialize a track
            free_track_parameters ray(ori, 0, dir, -1);

            tracks_host.push_back(ray);
            tracks_device.push_back(ray);
        }
    }

    /**
     * Host Volume Record
     */

    /*time*/ auto start_cpu = std::chrono::system_clock::now();

    vecmem::jagged_vector<dindex> volume_records_host(theta_steps * phi_steps,
                                                      &mng_mr);
    vecmem::jagged_vector<point3> position_records_host(theta_steps * phi_steps,
                                                        &mng_mr);

    for (unsigned int i = 0; i < theta_steps * phi_steps; i++) {

        auto& traj = tracks_host[i];
        navigator_host_t::state state(mng_mr);
        stepper_t::state stepping(traj);

        // Set initial volume
        state.set_volume(0u);

        // Start propagation and record volume IDs
        bool heartbeat = true;

        while (heartbeat) {
            heartbeat = n.target(state, stepping);

            stepping().set_pos(stepping().pos() + state() * stepping().dir());

            heartbeat = n.status(state, stepping);

            // Record volume
            volume_records_host[i].push_back(state.volume());
            position_records_host[i].push_back(stepping().pos());
        }
    }

    /*time*/ auto end_cpu = std::chrono::system_clock::now();
    /*time*/ std::chrono::duration<double> time_cpu = end_cpu - start_cpu;

    /**
     * Device Volume Record
     */

    /*time*/ auto start_cuda = std::chrono::system_clock::now();

    vecmem::jagged_vector<dindex> volume_records_device(&mng_mr);
    vecmem::jagged_vector<point3> position_records_device(&mng_mr);

    // Create size and capacity vectors for volume record buffer
    std::vector<size_t> sizes;
    std::vector<size_t> capacities;

    for (unsigned int i = 0; i < theta_steps * phi_steps; i++) {
        sizes.push_back(0);
        capacities.push_back(volume_records_host[i].size());
    }

    vecmem::data::jagged_vector_buffer<dindex> volume_records_buffer(
        sizes, capacities, dev_mr, &mng_mr);
    copy.setup(volume_records_buffer);

    vecmem::data::jagged_vector_buffer<point3> position_records_buffer(
        sizes, capacities, dev_mr, &mng_mr);
    copy.setup(position_records_buffer);

    // Get detector data
    auto det_data = get_data(det);

    // Get tracks data
    auto tracks_data = vecmem::get_data(tracks_device);

    // Create navigator candidates buffer
    auto candidates_buffer =
        create_candidates_buffer(det, theta_steps * phi_steps, dev_mr);
    copy.setup(candidates_buffer);

    // Run navigator test
    navigator_test(det_data, tracks_data, candidates_buffer,
                   volume_records_buffer, position_records_buffer);

    // Copy volume record buffer into volume & position records device
    copy(volume_records_buffer, volume_records_device);
    copy(position_records_buffer, position_records_device);

    for (unsigned int i = 0; i < volume_records_host.size(); i++) {

        EXPECT_EQ(volume_records_host[i].size(),
                  volume_records_device[i].size());

        for (unsigned int j = 0; j < volume_records_host[i].size(); j++) {

            EXPECT_EQ(volume_records_host[i][j], volume_records_device[i][j]);

            auto& pos_host = position_records_host[i][j];
            auto& pos_device = position_records_device[i][j];

            EXPECT_NEAR(pos_host[0], pos_device[0], pos_diff_tolerance);
            EXPECT_NEAR(pos_host[1], pos_device[1], pos_diff_tolerance);
            EXPECT_NEAR(pos_host[2], pos_device[2], pos_diff_tolerance);
        }
    }

    /*time*/ auto end_cuda = std::chrono::system_clock::now();
    /*time*/ std::chrono::duration<double> time_cuda = end_cuda - start_cuda;

    std::cout << "==> Elpased time ... " << std::endl;
    std::cout << "CPU: " << time_cpu.count() << std::endl;
    std::cout << "CUDA: " << time_cuda.count() << std::endl;
}