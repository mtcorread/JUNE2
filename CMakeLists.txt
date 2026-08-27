cmake_minimum_required(VERSION 3.14)
project(disease_simulation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ABI Compatibility Note:
# If you get "undefined reference" errors for HDF5 / yaml-cpp functions, it's likely an ABI mismatch.
# Uncomment the line below ONLY if your HDF5 / yaml-cpp were built with the old C++11 ABI (pre-GCC 5.1)
# For modern systems (GCC 5.1+), leave this commented out (use new ABI, which is the default)
# add_compile_definitions(_GLIBCXX_USE_CXX11_ABI=0)

# Find HDF5
if(USE_MPI)
  set(HDF5_PREFER_PARALLEL TRUE)
endif()

# Hints for Ubuntu's HDF5 directory structure
set(HDF5_INCLUDE_HINTS /usr/include/hdf5/serial /usr/include/hdf5/openmpi)

find_package(HDF5 REQUIRED COMPONENTS CXX)
if(HDF5_FOUND)
  # If HDF5_INCLUDE_DIRS doesn't include the hints on Ubuntu, add them
  foreach(hint ${HDF5_INCLUDE_HINTS})
    if(EXISTS ${hint})
      list(APPEND HDF5_INCLUDE_DIRS ${hint})
    endif()
  endforeach()
endif()

# Find yaml-cpp
# For HPC systems, prefer manual detection with YAMLCPP_DIR
if(YAMLCPP_DIR OR DEFINED ENV{YAMLCPP_DIR})
  if(YAMLCPP_DIR)
    set(YAML_CPP_ROOT ${YAMLCPP_DIR})
  else()
    set(YAML_CPP_ROOT $ENV{YAMLCPP_DIR})
  endif()
  message(STATUS "Using YAMLCPP_DIR: ${YAML_CPP_ROOT}")

  find_library(YAML_CPP_LIBRARIES NAMES yaml-cpp libyaml-cpp PATHS ${YAML_CPP_ROOT}/lib ${YAML_CPP_ROOT}/lib64 NO_DEFAULT_PATH)
  find_path(YAML_CPP_INCLUDE_DIR NAMES yaml-cpp/yaml.h PATHS ${YAML_CPP_ROOT}/include NO_DEFAULT_PATH)

  if(YAML_CPP_LIBRARIES AND YAML_CPP_INCLUDE_DIR)
    message(STATUS "yaml-cpp library: ${YAML_CPP_LIBRARIES}")
    message(STATUS "yaml-cpp include: ${YAML_CPP_INCLUDE_DIR}")
    set(yaml-cpp_FOUND TRUE)
  else()
    message(FATAL_ERROR "yaml-cpp not found in YAMLCPP_DIR: ${YAML_CPP_ROOT}")
  endif()
else()
  # Fall back to find_package for non-HPC systems
  find_package(yaml-cpp REQUIRED)
  message(STATUS "yaml-cpp found via find_package")
endif()

# Optional: Find MPI for parallel execution
option(USE_MPI "Enable MPI parallel execution" ON)
if(USE_MPI)
  find_package(MPI)
  if(MPI_FOUND)
    message(STATUS "MPI found: Parallel execution enabled")
  else()
    message(WARNING "MPI not found: Building serial version only")
  endif()
endif()

# Optional: Find METIS for graph partitioning (better than RCB)
option(USE_METIS "Enable METIS graph partitioning" ON)
if(USE_METIS)
  # Try to find METIS using CMake variable, environment variable, or brew prefix on macOS
  if(METIS_DIR)
    set(METIS_HINTS HINTS ${METIS_DIR}/lib ${METIS_DIR}/lib64 ${METIS_DIR}/include)
  elseif(DEFINED ENV{METIS_DIR})
    set(METIS_HINTS HINTS $ENV{METIS_DIR}/lib $ENV{METIS_DIR}/lib64 $ENV{METIS_DIR}/include)
  elseif(APPLE)
    execute_process(COMMAND brew --prefix metis OUTPUT_VARIABLE METIS_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(METIS_PREFIX)
      set(METIS_HINTS HINTS ${METIS_PREFIX}/lib ${METIS_PREFIX}/include)
    endif()
  endif()

  find_library(METIS_LIBRARY metis ${METIS_HINTS})
  find_path(METIS_INCLUDE_DIR metis.h ${METIS_HINTS})

  if(METIS_LIBRARY AND METIS_INCLUDE_DIR)
    message(STATUS "METIS found: ${METIS_LIBRARY}")
    message(STATUS "METIS include: ${METIS_INCLUDE_DIR}")
    add_definitions(-DUSE_METIS)
    set(METIS_FOUND TRUE)
  else()
    message(WARNING "METIS not found: Will use RCB partitioning as fallback")
    set(METIS_FOUND FALSE)
  endif()
endif()

# ==============================================================================
# Profiling options - Use gperftools (Google CPU Profiler)
# ==============================================================================
option(ENABLE_PROFILING "Enable CPU profiling with gperftools" ON)

# Find gperftools
if(ENABLE_PROFILING)
  if(GPERFTOOLS_DIR)
    set(GPERFTOOLS_HINTS HINTS ${GPERFTOOLS_DIR}/lib ${GPERFTOOLS_DIR}/lib64)
  elseif(DEFINED ENV{GPERFTOOLS_DIR})
    set(GPERFTOOLS_HINTS HINTS $ENV{GPERFTOOLS_DIR}/lib $ENV{GPERFTOOLS_DIR}/lib64)
  elseif(APPLE)
    set(GPERFTOOLS_HINTS HINTS /opt/homebrew/lib)
  endif()

  find_library(PROFILER_LIBRARY profiler ${GPERFTOOLS_HINTS})
  if(PROFILER_LIBRARY)
    message(STATUS "gperftools found: ${PROFILER_LIBRARY}")
    add_definitions(-DUSE_GPERFTOOLS)
    set(GPERFTOOLS_FOUND TRUE)
  else()
    message(WARNING "gperftools not found. Install with: brew install gperftools")
    set(GPERFTOOLS_FOUND FALSE)
  endif()
endif()

# ==============================================================================
# Core Simulation Library
# ==============================================================================
set(JUNE_CORE_SOURCES
    src/epidemiology/infection_seed.cpp
    src/epidemiology/seeding/seed_selector.cpp
    src/epidemiology/seeding/seed_shortfall.cpp
    src/epidemiology/seeding/seed_cluster_planner.cpp
    src/epidemiology/disease.cpp
    src/loaders/disease_loader.cpp
    src/interaction/interaction_manager.cpp
    src/interaction/interaction_manager_venue.cpp
    src/interaction/interaction_manager_venue_bins.cpp
    src/interaction/interaction_manager_partial_presence.cpp
    src/activity/activity_manager.cpp
    src/activity/activity_manager_selection.cpp
    src/activity/on_the_fly_venue_allocator.cpp
    src/core/config.cpp
    src/core/config_checks.cpp
    src/core/world_state.cpp
    src/activity/coordinated_encounter_manager.cpp
    src/activity/coordinated_encounter_manager_stats.cpp
    src/activity/runtime_group_allocator.cpp
    src/activity/presence_window.cpp
    src/utils/event_logging/event_logger.cpp
    src/utils/filtering.cpp
    src/utils/filtered_csv.cpp
    src/utils/event_logging/event_writer.cpp
    src/utils/event_logging/event_writer_lookups.cpp
    src/utils/mpi_logging.cpp
    src/utils/event_logging/event_merger.cpp
    src/utils/event_logging/event_merger_lookups.cpp
    src/epidemiology/epidemiology.cpp
    src/epidemiology/policy.cpp
    src/epidemiology/calendar_event.cpp
    src/loaders/calendar_event_loader.cpp
    src/loaders/catchment_rule_loader.cpp
    src/simulation/simulator.cpp
    src/simulation/encounters/eligibility.cpp
    src/simulation/simulator_checkpoint.cpp
    src/simulation/simulator_encounters.cpp
    src/simulation/simulator_reporting.cpp
    src/simulation/simulator_restore.cpp
    src/simulation/simulator_timeslot.cpp
    src/simulation/compartmental_model_manager.cpp
    src/loaders/hdf5_loader.cpp
    src/loaders/domain_loader.cpp
    src/loaders/domain_loader_internals.cpp
    src/loaders/config_loader.cpp
    src/loaders/config_loader_encounters.cpp
    src/loaders/config_loader_vaccination.cpp)

if(MPI_FOUND)
  list(APPEND JUNE_CORE_SOURCES
       src/parallel/domain.cpp
       src/parallel/domain_manager.cpp
       src/parallel/seed_offer_exchange_mpi.cpp
       src/parallel/geography_partitioner.cpp
       src/parallel/domain_communicator.cpp
       src/parallel/domain_communicator_encounters.cpp)
endif()

# Create the library
add_library(june_core STATIC ${JUNE_CORE_SOURCES})

# GCC 15 false-positive: variant copy-assign with vector<int> triggers a
# spurious -Wfree-nonheap-object due to broken alias analysis in the compiler.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(june_core PRIVATE -Wno-free-nonheap-object)
endif()

# Define include directories for the library
# PUBLIC means anyone linking to june_core gets these headers too
target_include_directories(june_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_include_directories(june_core PUBLIC ${HDF5_INCLUDE_DIRS})

if(YAML_CPP_INCLUDE_DIR)
  target_include_directories(june_core PUBLIC ${YAML_CPP_INCLUDE_DIR})
endif()

if(MPI_FOUND)
  target_include_directories(june_core PUBLIC ${MPI_CXX_INCLUDE_DIRS})
  target_compile_definitions(june_core PUBLIC USE_MPI)
endif()

if(METIS_FOUND)
  target_include_directories(june_core PRIVATE ${METIS_INCLUDE_DIR})
  target_compile_definitions(june_core PRIVATE USE_METIS)
endif()

# Link mandatory libraries
target_link_libraries(june_core PUBLIC dl)

if(TARGET yaml-cpp::yaml-cpp)
  target_link_libraries(june_core PUBLIC yaml-cpp::yaml-cpp)
elseif(YAML_CPP_LIBRARIES)
  target_link_libraries(june_core PUBLIC ${YAML_CPP_LIBRARIES})
endif()

if(HDF5_CXX_LIBRARIES)
  target_link_libraries(june_core PRIVATE ${HDF5_CXX_LIBRARIES})
endif()
if(HDF5_LIBRARIES)
  target_link_libraries(june_core PRIVATE ${HDF5_LIBRARIES})
endif()
# HDF5_CXX_LIBRARIES is unpopulated by some CMake/HDF5 versions even when
# find_package(HDF5 COMPONENTS CXX) succeeds. Link the C++ wrapper explicitly
# when the individual component path is known; without it, C++ API symbols
# (H5::DSetCreatPropList, H5::StrType, etc.) are undefined at link time.
message(STATUS "HDF5_CXX_LIBRARIES = [${HDF5_CXX_LIBRARIES}]")
message(STATUS "HDF5_LIBRARIES = [${HDF5_LIBRARIES}]")
if(NOT HDF5_CXX_LIBRARIES AND HDF5_CXX_LIBRARY_hdf5_cpp)
  target_link_libraries(june_core PRIVATE ${HDF5_CXX_LIBRARY_hdf5_cpp})
endif()

if(MPI_FOUND)
  target_link_libraries(june_core PUBLIC MPI::MPI_CXX)
endif()

if(METIS_FOUND)
  target_link_libraries(june_core PRIVATE ${METIS_LIBRARY})
endif()

if(GPERFTOOLS_FOUND)
  target_link_libraries(june_core PRIVATE ${PROFILER_LIBRARY})
  target_compile_definitions(june_core PRIVATE USE_GPERFTOOLS)
endif()

# ==============================================================================
# Main Executable
# ==============================================================================
add_executable(disease_sim src/main.cpp)
target_link_libraries(disease_sim PRIVATE june_core)

add_executable(merge_events src/merge_events.cpp)
target_link_libraries(merge_events PRIVATE june_core)

if(GPERFTOOLS_FOUND)
  message(STATUS "Profiling enabled: gperftools CPU profiler linked via june_core")
endif()

# ==============================================================================
# Unit Tests
# ==============================================================================
option(BUILD_TESTING "Build unit tests" ON)

if(BUILD_TESTING)
  enable_testing()

  # helper for adding tests
  macro(add_june_test name sources)
    add_executable(${name} ${sources})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(${name} PRIVATE june_core)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endmacro()

  # Macro to add MPI-enabled tests
  macro(add_june_mpi_test name sources)
    add_executable(${name} ${sources})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(${name} PRIVATE june_core)

    # Find mpirun or equivalent
    if(NOT MPIEXEC_EXECUTABLE)
      find_program(MPIEXEC_EXECUTABLE NAMES mpirun mpiexec)
    endif()

    if(MPIEXEC_EXECUTABLE)
      add_test(NAME ${name} COMMAND ${MPIEXEC_EXECUTABLE} -np 2 --oversubscribe ${CMAKE_CURRENT_BINARY_DIR}/${name} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    else()
      message(FATAL_ERROR "MPI execution command (mpirun/mpiexec) not found, but MPI tests are enabled. Cannot configure ${name}.")
    endif()
  endmacro()

  add_june_test(test_domain_communicator_detail "tests/test_domain_communicator_detail.cpp")
  add_june_test(test_run_dir "tests/test_run_dir.cpp")
  add_june_test(test_world_state "tests/test_world_state.cpp")
  add_june_test(test_selection_criterion_geography "tests/test_selection_criterion_geography.cpp")
  add_june_test(test_domain_loader_internals "tests/test_domain_loader_internals.cpp")
  add_june_test(test_infection "tests/test_infection.cpp;src/loaders/disease_loader.cpp")
  add_june_test(test_transmission_lookups "tests/test_transmission_lookups.cpp")
  add_june_test(test_coordinated_encounters "tests/test_coordinated_encounters.cpp")
  add_june_test(test_follow "tests/test_follow.cpp")
  add_june_test(test_fomite_transmission "tests/test_fomite_transmission.cpp;src/loaders/disease_loader.cpp")
  add_june_test(test_compartmental_disease_mode "tests/test_compartmental_disease_mode.cpp;src/loaders/disease_loader.cpp")
  add_june_test(test_compartmental_coupling_matrix "tests/test_compartmental_coupling_matrix.cpp")
  add_june_test(test_compartmental_transmission "tests/test_compartmental_transmission.cpp")
  add_june_test(test_compartmental_writeback "tests/test_compartmental_writeback.cpp")
  add_june_test(test_compartmental_integration "tests/test_compartmental_integration.cpp")
  add_june_test(test_vaccines "tests/test_vaccines.cpp")
  add_june_test(test_vaccination_manager "tests/test_vaccination_manager.cpp")
  add_june_test(test_config_loading "tests/test_config_loading.cpp")
  add_june_test(test_config_checks "tests/test_config_checks.cpp")
  add_june_test(test_activity_manager "tests/test_activity_manager.cpp")
  add_june_test(test_policy "tests/test_policy.cpp")
  add_june_test(test_on_the_fly_venue_allocator "tests/test_on_the_fly_venue_allocator.cpp")
  add_june_test(test_schedule_csv "tests/test_schedule_csv.cpp")
  add_june_test(test_day_types "tests/test_day_types.cpp")
  add_june_test(test_schedule_hopper "tests/test_schedule_hopper.cpp")
  add_june_test(test_schedule_hop_state "tests/test_schedule_hop_state.cpp")
  add_june_test(test_calendar_event_trigger    "tests/test_calendar_event_trigger.cpp")
  add_june_test(test_calendar_event_loader     "tests/test_calendar_event_loader.cpp")
  add_june_test(test_calendar_event_venue      "tests/test_calendar_event_venue.cpp")
  add_june_test(test_calendar_event_checkpoint "tests/test_calendar_event_checkpoint.cpp")
  add_june_test(test_catchment_rule_loader "tests/test_catchment_rule_loader.cpp")
  add_june_test(test_infection_seed_loader "tests/test_infection_seed_loader.cpp")
  add_june_test(test_seed_selector "tests/test_seed_selector.cpp")
  add_june_test(test_seed_shortfall "tests/test_seed_shortfall.cpp")
  add_june_test(test_seed_cluster_planner "tests/test_seed_cluster_planner.cpp")
  add_june_test(test_cross_domain_venues "tests/test_cross_domain_venues.cpp")
  add_june_test(test_membership_metadata_matching "tests/test_membership_metadata_matching.cpp")
  add_june_test(test_integration "tests/test_integration.cpp;src/loaders/disease_loader.cpp")
  add_june_test(test_epidemiology "tests/test_epidemiology.cpp;src/loaders/disease_loader.cpp")
  add_june_test(test_simulator "tests/test_simulator.cpp;src/loaders/disease_loader.cpp")
  # Mock compartmental model plugin .so used by compartmental model manager tests.
  add_library(mock_compartmental_model SHARED tests/mock_compartmental_model.cpp)
  target_include_directories(mock_compartmental_model PRIVATE ${CMAKE_SOURCE_DIR}/include)

  add_june_test(test_compartmental_model_manager "tests/test_compartmental_model_manager.cpp")
  # Pass the built .so path so the test can load it without hardcoding.
  target_compile_definitions(test_compartmental_model_manager PRIVATE
      MOCK_COMPARTMENTAL_MODEL_SO_PATH="$<TARGET_FILE:mock_compartmental_model>")
  add_dependencies(test_compartmental_model_manager mock_compartmental_model)
  # Slice 3: test writes synthetic HDF5 graphs using the C API directly.
  if(HDF5_CXX_LIBRARY_hdf5)
    target_link_libraries(test_compartmental_model_manager PRIVATE ${HDF5_CXX_LIBRARY_hdf5})
  endif()
  add_june_test(test_transmission_modes "tests/test_transmission_modes.cpp;src/loaders/disease_loader.cpp")
  add_june_test(test_deterministic_rng "tests/test_deterministic_rng.cpp")
  add_june_test(test_transmission_determinism "tests/test_transmission_determinism.cpp;src/loaders/disease_loader.cpp")
  add_june_test(test_foi_calculation "tests/test_foi_calculation.cpp;src/loaders/disease_loader.cpp")
  add_june_test(test_presence_window "tests/test_presence_window.cpp")
  add_june_test(test_partial_presence_foi "tests/test_partial_presence_foi.cpp;src/loaders/disease_loader.cpp")

  if(MPI_FOUND)
    add_june_mpi_test(test_mpi_communication "tests/test_mpi_communication.cpp")
    add_june_mpi_test(test_mpi_advanced "tests/test_mpi_advanced.cpp")
    add_june_mpi_test(test_mpi_transmission_modes "tests/test_mpi_transmission_modes.cpp")

    if(MPIEXEC_EXECUTABLE)
      # End-to-end disease_sim reproducibility: runs disease_sim at
      # np=1/2/3 against the real config + world and h5diff's the
      # /events/relationships, /events/coordinated_encounters (minus
      # partition-dependent group_id), and /events/infections datasets.
      # Covers R2 (OOE roster), E2 (coordinated encounters), and
      # infection counts bit-identical across np. Slow (~30s at 5 days);
      # opt out via ctest -E mpi_full_repro.
      add_test(NAME test_mpi_full_reproducibility
               COMMAND ${CMAKE_SOURCE_DIR}/tests/test_mpi_full_reproducibility.sh
               WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
      set_tests_properties(test_mpi_full_reproducibility PROPERTIES
                           ENVIRONMENT "BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};DAYS=5"
                           SKIP_RETURN_CODE 77 TIMEOUT 600)

      # The same end-to-end diff, seeded structurally above the partition
      # level (an exact seed in an LGU and in an XLGU, MGU being where
      # parallel.yaml partitions). A structured seed states an absolute
      # count, so this is what catches a unit being seeded once per rank
      # covering it.
      add_test(NAME test_mpi_structured_seed_reproducibility
               COMMAND ${CMAKE_SOURCE_DIR}/tests/test_mpi_full_reproducibility.sh
               WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
      set_tests_properties(test_mpi_structured_seed_reproducibility PROPERTIES
                           ENVIRONMENT "BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR};DAYS=3;SEEDS=configs/config_2021/infection_seeds_structured.yaml"
                           SKIP_RETURN_CODE 77 TIMEOUT 600)

      # Checkpoint/restart determinism gate: a checkpoint leg + resume leg
      # must be set-identical to an uninterrupted baseline across a
      # (write_ranks -> resume_ranks) matrix, incl. asymmetric resume. Skips
      # cleanly (exit 77) if the world file or venv python is unavailable
      # (e.g. CI without data). Opt out via: ctest -E checkpoint_determinism.
      find_program(JUNE_VENV_PYTHON
                   NAMES python3
                   PATHS ${CMAKE_SOURCE_DIR}/.venv/bin NO_DEFAULT_PATH)
      if(JUNE_VENV_PYTHON)
        add_test(NAME test_checkpoint_determinism
                 COMMAND ${JUNE_VENV_PYTHON}
                         ${CMAKE_SOURCE_DIR}/tests/checkpoint_determinism_check.py
                         --bin ${CMAKE_CURRENT_BINARY_DIR}/disease_sim
                         --config configs/config_1911/simulation.yaml
                         --world worlds/1911_durham_world.h5
                         --days 4 --ckpt-day 2
                         --matrix 1:1,2:2,2:1
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(test_checkpoint_determinism PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 900)
      endif()
    endif()
  endif()
endif()
