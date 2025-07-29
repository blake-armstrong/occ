#include <fmt/os.h>
#include <fmt/ostream.h>
#include <occ/core/eeq.h>
#include <occ/core/log.h>
#include <occ/core/units.h>
#include <occ/crystal/crystal.h>
#include <occ/io/adaptive_grid.h>
#include <occ/io/cifparser.h>
#include <occ/io/cube.h>
#include <occ/io/load_geometry.h>
#include <occ/io/periodic_grid.h>
#include <occ/io/xyz.h>
#include <occ/main/occ_cube.h>
#include <occ/main/point_functors.h>
#include <occ/qm/wavefunction.h>
#include <scn/scan.h>
#include <memory>

namespace fs = std::filesystem;
using occ::IVec;
using occ::Mat3N;
using occ::Vec;
using occ::Vec3;
using occ::core::Element;
using occ::core::Molecule;
using occ::io::Cube;
using occ::qm::Wavefunction;

namespace occ::main {

void require_wfn(const CubeConfig &config, bool have_wfn) {
  if (have_wfn)
    return;
  throw std::runtime_error("Property requires a wavefunction: " +
                           config.property);
}

CLI::App *add_cube_subcommand(CLI::App &app) {

  CLI::App *cube =
      app.add_subcommand("cube", "compute molecule/qm properties on points");
  auto config = std::make_shared<CubeConfig>();

  cube->add_option("input", config->input_filename,
                   "input geometry file (xyz, wavefunction ...)")
      ->required();

  cube->add_option("property", config->property,
                   "property to evaluate (default=density)");

  cube->add_option(
      "spin", config->spin,
      "spin (for e.g. electron density) [alpha,beta,default=both]");

  cube->add_option(
      "--mo", config->mo_number,
      "MO number (for e.g. electron density) [default=-1 i.e. all]");
  cube->add_option("--functional", config->functional,
                   "DFT functional for XC density [default=blyp]");

  cube->add_option("-n,--steps", config->steps,
                   "Number of steps in each direction")
      ->expected(0, 3)
      ->default_val("{}");

  cube->add_option(
      "--points", config->points_filename,
      "list of points/mesh file requesting points to evaluate the property");
  cube->add_option("--output,-o", config->output_filename,
                   "destination to write file");

  cube->add_option("--origin", config->origin, "origin/starting point for grid")
      ->expected(0, 3)
      ->default_val("{}");

  cube->add_option("--direction-a,--direction_a,--da,-a", config->da,
                   "Translation in direction A")
      ->expected(0, 3)
      ->default_val("{}");
  cube->add_option("--direction-b,--direction_b,--db,-b", config->db,
                   "Translation in direction B")
      ->expected(0, 3)
      ->default_val("{}");
  cube->add_option("--direction-c,--direction_c,--dc,-c", config->dc,
                   "Translation in direction C")
      ->expected(0, 3)
      ->default_val("{}");

  cube->add_flag("--adaptive", config->adaptive_bounds,
                 "Use adaptive bounds calculation");
  cube->add_option("--threshold", config->value_threshold,
                   "Value threshold for adaptive bounds (default=1e-8)");
  cube->add_option("--buffer", config->buffer_distance,
                   "Buffer distance for adaptive bounds in Angstrom (default=2.0)");
  cube->add_option("--format", config->format,
                   "Output format: cube, ggrid, pgrid (default=cube)");
  
  cube->add_option("--crystal", config->crystal_filename,
                   "CIF file for crystal structure (enables symmetry-aware pgrid generation)");
  cube->add_flag("--unit-cell", config->unit_cell_only,
                 "Generate grid for unit cell only (requires --crystal)");

  cube->fallthrough();
  cube->callback([config]() { run_cube_subcommand(*config); });
  return cube;
}

inline void fail_with_error(const std::string &msg, int line) {
  throw std::runtime_error(fmt::format(
      "Unable to parse points file, error: {}, line {}", msg, line));
}

Mat3N read_points_file(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    fail_with_error(fmt::format("Failed to open points file '{}'", filename),
                    0);
  }

  std::string line;
  std::getline(file, line);

  auto result = scn::scan<int>(line, "{}");
  if (!result) {
    fail_with_error(result.error().msg(), 0);
  }
  int n = result->value();

  Mat3N points(3, n);

  for (int i = 0; i < n; ++i) {
    std::getline(file, line);
    auto scan_result = scn::scan<double, double, double>(line, "{} {} {}");
    if (!scan_result) {
      fail_with_error(scan_result.error().msg(), i + 1);
    }
    auto [px, py, pz] = scan_result->values();
    points(0, i) = px;
    points(1, i) = py;
    points(2, i) = pz;
  }

  return points;
}

Mat3N load_points_for_evaluation(const std::string &points_filename) {
  return read_points_file(points_filename);
}

void write_results(CubeConfig const &config, const Mat3N &points,
                   const Vec &data) {
  auto output = fmt::output_file(
      config.output_filename, fmt::file::WRONLY | O_TRUNC | fmt::file::CREATE);
  output.print("{}\n", data.rows());
  for (int i = 0; i < data.rows(); i++) {
    output.print("{:20.12f} {:20.12f} {:20.12f} {:20.12f}\n", points(0, i),
                 points(1, i), points(2, i), data(i));
  }
}

void evaluate_custom_points(const Wavefunction &wfn, CubeConfig const &config,
                            bool have_wfn) {

  auto points = load_points_for_evaluation(config.points_filename);
  Vec data = Vec::Zero(points.cols());

  if (config.mo_number > -1) {
    occ::log::info("Specified MO number:    {}", config.mo_number);
  }
  occ::timing::start(occ::timing::category::cube_evaluation);
  if (config.property == "eeqesp") {
    EEQEspFunctor func(wfn.atoms);
    func(points, data);
  } else if (config.property == "promolecule") {
    PromolDensityFunctor func(wfn.atoms);
    func(points, data);
  } else if (config.property == "deformation_density") {
    require_wfn(config, have_wfn);
    DeformationDensityFunctor func(wfn);
    func(points, data);
  } else if (config.property == "rho" ||
             config.property == "electron_density" ||
             config.property == "density") {
    require_wfn(config, have_wfn);
    ElectronDensityFunctor func(wfn);
    func.mo_index = config.mo_number;
    func(points, data);
  } else if (config.property == "rho_alpha") {
    require_wfn(config, have_wfn);
    ElectronDensityFunctor func(wfn);
    func.spin = SpinConstraint::Alpha;
    func.mo_index = config.mo_number;
    func(points, data);
  } else if (config.property == "rho_beta") {
    require_wfn(config, have_wfn);
    ElectronDensityFunctor func(wfn);
    func.spin = SpinConstraint::Beta;
    func.mo_index = config.mo_number;
    func(points, data);
  } else if (config.property == "esp") {
    require_wfn(config, have_wfn);
    EspFunctor func(wfn);
    func(points, data);
  } else if (config.property == "xc") {
    require_wfn(config, have_wfn);
    XCDensityFunctor func(wfn, config.functional);
    func(points, data);
  } else
    throw std::runtime_error(
        fmt::format("Unknown property to evaluate: {}", config.property));
  occ::timing::stop(occ::timing::category::cube_evaluation);

  write_results(config, points, data);
}

void run_cube_subcommand(CubeConfig const &config_in) {
  // Make a mutable copy so we can modify for convenience features
  CubeConfig config = config_in;
  Wavefunction wfn;
  bool have_wfn{false};

  // Handle crystal if requested - store for later use
  std::unique_ptr<occ::crystal::Crystal> crystal_ptr;
  
  // Auto-detect CIF files
  bool is_cif_input = occ::io::CifParser::is_likely_cif_filename(config.input_filename);
  if (is_cif_input && config.crystal_filename.empty()) {
    // If input is a CIF and no separate crystal file specified, use input as crystal
    config.crystal_filename = config.input_filename;
  }

  if (Wavefunction::is_likely_wavefunction_filename(config.input_filename)) {
    wfn = Wavefunction::load(config.input_filename);
    occ::log::info("Loaded wavefunction from {}", config.input_filename);
    occ::log::info("Spinorbital kind: {}",
                   occ::qm::spinorbital_kind_to_string(wfn.mo.kind));
    occ::log::info("Num alpha:        {}", wfn.mo.n_alpha);
    occ::log::info("Num beta:         {}", wfn.mo.n_beta);
    occ::log::info("Num AOs:          {}", wfn.mo.n_ao);
    have_wfn = true;
  } else if (!is_cif_input) {
    Molecule m = occ::io::molecule_from_xyz_file(config.input_filename);
    wfn.atoms = m.atoms();
  }
  if (!config.crystal_filename.empty()) {
    occ::log::info("Loading crystal structure from {}", config.crystal_filename);
    crystal_ptr = std::make_unique<occ::crystal::Crystal>(occ::io::load_crystal(config.crystal_filename));
    
    // For crystals, we need to set up the grid to match the unit cell
    // and atoms from the asymmetric unit
    const auto &cell = crystal_ptr->unit_cell();
    const auto &asym_unit = crystal_ptr->asymmetric_unit();
    
    // For void calculations, we need unit cell atoms + neighbors
    if (config.property == "void" || config.property == "promolecule") {
      // Get atoms within a buffer radius around the unit cell
      double buffer = 6.0;  // 6 Angstrom buffer
      const auto &uc_atoms = crystal_ptr->unit_cell_atoms();
      occ::crystal::HKL upper = occ::crystal::HKL::minimum();
      occ::crystal::HKL lower = occ::crystal::HKL::maximum();
      Vec3 frac_radius = buffer * 2 / cell.lengths().array();
      
      for (size_t i = 0; i < uc_atoms.frac_pos.cols(); i++) {
        const auto &pos = uc_atoms.frac_pos.col(i);
        upper.h = std::max(upper.h, static_cast<int>(ceil(pos(0) + frac_radius(0))));
        upper.k = std::max(upper.k, static_cast<int>(ceil(pos(1) + frac_radius(1))));
        upper.l = std::max(upper.l, static_cast<int>(ceil(pos(2) + frac_radius(2))));
        
        lower.h = std::min(lower.h, static_cast<int>(floor(pos(0) - frac_radius(0))));
        lower.k = std::min(lower.k, static_cast<int>(floor(pos(1) - frac_radius(1))));
        lower.l = std::min(lower.l, static_cast<int>(floor(pos(2) - frac_radius(2))));
      }
      
      auto slab = crystal_ptr->slab(lower, upper);
      
      // Convert slab atoms to wavefunction atoms (convert from Angstrom to Bohr)
      wfn.atoms.clear();
      for (size_t i = 0; i < slab.atomic_numbers.size(); i++) {
        wfn.atoms.push_back(occ::core::Atom{
          slab.atomic_numbers(i),
          slab.cart_pos(0, i) * occ::units::ANGSTROM_TO_BOHR,
          slab.cart_pos(1, i) * occ::units::ANGSTROM_TO_BOHR,
          slab.cart_pos(2, i) * occ::units::ANGSTROM_TO_BOHR
        });
      }
      
      occ::log::info("Using {} atoms (unit cell + neighbors within {:.1f} Å)", 
                     wfn.atoms.size(), buffer);
    } else {
      // For other properties, just use asymmetric unit atoms
      wfn.atoms.clear();
      for (size_t i = 0; i < asym_unit.size(); i++) {
        // Positions in asymmetric unit are fractional, need to convert to Cartesian
        Vec3 cart_pos = cell.direct() * asym_unit.positions.col(i);
        wfn.atoms.push_back(occ::core::Atom{
          asym_unit.atomic_numbers(i),
          cart_pos(0) * occ::units::ANGSTROM_TO_BOHR,
          cart_pos(1) * occ::units::ANGSTROM_TO_BOHR,
          cart_pos(2) * occ::units::ANGSTROM_TO_BOHR
        });
      }
    }
    
    occ::log::info("Crystal has {} atoms in asymmetric unit", wfn.atoms.size());
    occ::log::info("Unit cell parameters: a={:.3f} b={:.3f} c={:.3f} alpha={:.1f} beta={:.1f} gamma={:.1f}",
                   cell.a(), cell.b(), cell.c(), cell.alpha(), cell.beta(), cell.gamma());
  }
  
  // Handle void as an alias for promolecule when using crystals
  if (config.property == "void") {
    if (!crystal_ptr) {
      throw std::runtime_error("Void calculation requires a crystal structure (use --crystal)");
    }
    config.property = "promolecule";
    config.unit_cell_only = true;  // Voids are typically calculated for unit cell
    if (config.format == "cube") {
      config.format = "pgrid";  // Default to pgrid for crystal voids
    }
    // Also update output filename if it ends with .cube
    if (config.output_filename.ends_with(".cube")) {
      config.output_filename = config.output_filename.substr(0, config.output_filename.length() - 5) + ".pgrid";
    }
    occ::log::info("Void calculation: using promolecule density for unit cell");
  }

  Cube cube;
  cube.origin = Vec3::Zero();

  auto fill_values = [](const auto &vector, auto &dest) {
    if (vector.size() == 1) {
      dest.setConstant(vector[0]);
    } else {
      for (int i = 0; i < std::min(size_t{3}, vector.size()); i++) {
        dest(i) = vector[i];
      }
    }
  };
  auto fill_direction = [&cube](const auto &vector, int pos) {
    if (vector.size() == 1) {
      cube.basis(pos, pos) = vector[0] / cube.steps(0);
    } else {
      for (int i = 0; i < std::min(size_t{3}, vector.size()); i++) {
        cube.basis(i, pos) = vector[i] / cube.steps(i);
      }
    }
  };
  cube.atoms = wfn.atoms;

  // If crystal is loaded and unit cell requested, set up grid for unit cell
  if (crystal_ptr && config.unit_cell_only) {
    const auto &cell = crystal_ptr->unit_cell();
    
    // Set origin to 0,0,0 (unit cell corner)
    cube.origin = Vec3::Zero();
    
    // Set basis vectors from unit cell (convert to Bohr)
    cube.basis = cell.direct() * occ::units::ANGSTROM_TO_BOHR;
    
    // Set steps from config or use defaults
    fill_values(config.steps, cube.steps);
    if (config.steps.empty()) {
      cube.steps = IVec3::Constant(50);  // Default 50 points per dimension
    }
    
    // Divide basis by steps to get spacing
    for (int i = 0; i < 3; i++) {
      cube.basis.col(i) /= cube.steps(i);
    }
    
    occ::log::info("Using unit cell dimensions for grid");
  }
  // Handle adaptive bounds if requested
  else if (config.adaptive_bounds) {
    occ::log::info("Using adaptive bounds calculation");
    
    // Convert buffer distance to Bohr
    // Create the appropriate functor based on property
    if (config.property == "eeqesp") {
      EEQEspFunctor func(wfn.atoms);
      occ::io::AdaptiveGridBounds<EEQEspFunctor>::Parameters adaptive_params;
      adaptive_params.value_threshold = config.value_threshold;
      adaptive_params.extra_buffer = config.buffer_distance * occ::units::ANGSTROM_TO_BOHR;
      
      auto bounds_calc = occ::io::make_adaptive_bounds(func, adaptive_params);
      Molecule mol(wfn.atoms);
      auto bounds = bounds_calc.compute(mol);
      
      cube.origin = bounds.origin;
      cube.basis = bounds.basis;
      cube.steps = bounds.steps;
    } else if (config.property == "promolecule") {
      PromolDensityFunctor func(wfn.atoms);
      occ::io::AdaptiveGridBounds<PromolDensityFunctor>::Parameters adaptive_params;
      adaptive_params.value_threshold = config.value_threshold;
      adaptive_params.extra_buffer = config.buffer_distance * occ::units::ANGSTROM_TO_BOHR;
      
      auto bounds_calc = occ::io::make_adaptive_bounds(func, adaptive_params);
      Molecule mol(wfn.atoms);
      auto bounds = bounds_calc.compute(mol);
      
      cube.origin = bounds.origin;
      cube.basis = bounds.basis;
      cube.steps = bounds.steps;
    } else if (config.property == "rho" || config.property == "electron_density" || 
               config.property == "density") {
      require_wfn(config, have_wfn);
      ElectronDensityFunctor func(wfn);
      func.mo_index = config.mo_number;
      occ::io::AdaptiveGridBounds<ElectronDensityFunctor>::Parameters adaptive_params;
      adaptive_params.value_threshold = config.value_threshold;
      adaptive_params.extra_buffer = config.buffer_distance * occ::units::ANGSTROM_TO_BOHR;
      
      auto bounds_calc = occ::io::make_adaptive_bounds(func, adaptive_params);
      Molecule mol(wfn.atoms);
      auto bounds = bounds_calc.compute(mol);
      
      cube.origin = bounds.origin;
      cube.basis = bounds.basis;
      cube.steps = bounds.steps;
    } else if (config.property == "esp") {
      require_wfn(config, have_wfn);
      EspFunctor func(wfn);
      occ::io::AdaptiveGridBounds<EspFunctor>::Parameters adaptive_params;
      adaptive_params.value_threshold = config.value_threshold;
      adaptive_params.extra_buffer = config.buffer_distance * occ::units::ANGSTROM_TO_BOHR;
      
      auto bounds_calc = occ::io::make_adaptive_bounds(func, adaptive_params);
      Molecule mol(wfn.atoms);
      auto bounds = bounds_calc.compute(mol);
      
      cube.origin = bounds.origin;
      cube.basis = bounds.basis;
      cube.steps = bounds.steps;
    } else {
      throw std::runtime_error("Adaptive bounds not supported for property: " + config.property);
    }
    
    occ::log::info("Adaptive bounds determined:");
    occ::log::info("  Origin: [{:.3f}, {:.3f}, {:.3f}] Bohr", 
                   cube.origin(0), cube.origin(1), cube.origin(2));
    occ::log::info("  Steps: {} x {} x {}", cube.steps(0), cube.steps(1), cube.steps(2));
    occ::log::info("  Spacing: [{:.3f}, {:.3f}, {:.3f}] Bohr",
                   cube.basis(0,0), cube.basis(1,1), cube.basis(2,2));
  } else {
    // Use manual specification
    fill_values(config.steps, cube.steps);

    cube.basis.diagonal().array() = 0.2;

    fill_direction(config.da, 0);
    fill_direction(config.db, 1);
    fill_direction(config.dc, 2);

    // Don't center molecule if using crystal coordinates
    if (!crystal_ptr) {
      cube.center_molecule();
    }
    fill_values(config.origin, cube.origin);
  }

  cube.name =
      fmt::format("Generated by OCC from file: {}", config.input_filename);
  cube.description =
      fmt::format("Scalar values for property '{}'", config.property);

  occ::log::info("Cube geometry");

  occ::log::info("Origin: {:12.5f} {:12.5f} {:12.5f}", cube.origin(0),
                 cube.origin(1), cube.origin(2));
  occ::log::info("A:{: 5d} {:12.5f} {:12.5f} {:12.5f}", cube.steps(0),
                 cube.basis(0, 0), cube.basis(1, 0), cube.basis(2, 0));
  occ::log::info("B:{: 5d} {:12.5f} {:12.5f} {:12.5f}", cube.steps(1),
                 cube.basis(0, 1), cube.basis(1, 1), cube.basis(2, 1));
  occ::log::info("C:{: 5d} {:12.5f} {:12.5f} {:12.5f}", cube.steps(2),
                 cube.basis(0, 2), cube.basis(1, 2), cube.basis(2, 2));

  if (config.mo_number > -1) {
    occ::log::info("Specified MO number:    {}", config.mo_number);
  }
  occ::timing::start(occ::timing::category::cube_evaluation);
  if (config.property == "eeqesp") {
    EEQEspFunctor func(wfn.atoms);
    cube.fill_data_from_function(func);
  } else if (config.property == "promolecule") {
    PromolDensityFunctor func(wfn.atoms);
    cube.fill_data_from_function(func);
  } else if (config.property == "deformation_density") {
    require_wfn(config, have_wfn);
    DeformationDensityFunctor func(wfn);
    cube.fill_data_from_function(func);
  } else if (config.property == "rho" ||
             config.property == "electron_density" ||
             config.property == "density") {
    require_wfn(config, have_wfn);
    ElectronDensityFunctor func(wfn);
    func.mo_index = config.mo_number;
    cube.fill_data_from_function(func);
  } else if (config.property == "rho_alpha") {
    require_wfn(config, have_wfn);
    ElectronDensityFunctor func(wfn);
    func.spin = SpinConstraint::Alpha;
    func.mo_index = config.mo_number;
    cube.fill_data_from_function(func);
  } else if (config.property == "rho_beta") {
    require_wfn(config, have_wfn);
    ElectronDensityFunctor func(wfn);
    func.spin = SpinConstraint::Beta;
    func.mo_index = config.mo_number;
    cube.fill_data_from_function(func);
  } else if (config.property == "esp") {
    require_wfn(config, have_wfn);
    EspFunctor func(wfn);
    cube.fill_data_from_function(func);
  } else if (config.property == "xc") {
    require_wfn(config, have_wfn);
    XCDensityFunctor func(wfn, config.functional);
    cube.fill_data_from_function(func);
  } else
    throw std::runtime_error(
        fmt::format("Unknown property to evaluate: {}", config.property));
  occ::timing::stop(occ::timing::category::cube_evaluation);

  occ::timing::start(occ::timing::category::io);
  
  // Save in the requested format
  if (config.format == "cube") {
    cube.save(config.output_filename);
  } else if (config.format == "ggrid" || config.format == "pgrid") {
    // Convert to Grid format
    auto grid_format = (config.format == "ggrid") ? 
                       occ::io::GridFormat::GeneralGrid : 
                       occ::io::GridFormat::PeriodicGrid;
    
    auto grid = occ::io::PeriodicGrid::from_cube(cube, grid_format);
    grid.save(config.output_filename);
    
    occ::log::info("Saved {} file: {}", config.format, config.output_filename);
  } else {
    throw std::runtime_error("Unknown output format: " + config.format);
  }
  
  occ::timing::stop(occ::timing::category::io);
}

} // namespace occ::main
