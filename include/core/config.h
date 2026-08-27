#pragma once

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "types.h"

namespace june {

// Forward declarations
struct Person;
struct WorldState;

// =============================================================================
// Schedule Selection Criteria
// =============================================================================

struct SelectionCriterion {
  std::string
      property_path;  // e.g., "age", "activities.primary_activity.length"
  std::string
      operator_type;    // ">", "<", "==", "!=", ">=", "<=", "in", "contains"
  PropertyValue value;  // Can be int, double, string, or vector

  // Evaluate this criterion against a person
  bool evaluate(const Person& person, const WorldState* world = nullptr,
                const Person* partner = nullptr) const;

  // Resolve string values to codes for early interning
  void resolve(const WorldState& world);

  // Resolve, and refuse anything this world cannot answer: a property path the
  // engine does not recognise, a properties.* name the world does not carry, or
  // an operator that is not implemented. Any of those would otherwise evaluate
  // false for every person, so a misspelt filter reads as "nobody qualifies"
  // instead of as the config error it is. `context` names the offending config
  // block in the error message.
  void resolveOrThrow(const WorldState& world, const std::string& context);

  // True for the property paths whose `value` is a geographical unit name (or
  // a list of them) rather than a number: `geo_unit.<LEVEL>` and nothing else.
  // Lexical, so config loaders can ask before a world exists. Kept beside the
  // path dispatch in resolve() so the two cannot drift.
  static bool comparesAgainstUnitNames(const std::string& property_path);

 private:
  enum class PropertyType {
    UNKNOWN,
    AGE,
    SEX,
    GEO_ID,
    PERSON_ID,
    ACTIVITY_LENGTH,
    ACTIVITY_VENUE_TYPE,
    CUSTOM_PROPERTY,
    NETWORK_SIZE,
    PARTNER_IN_NETWORK,
    // is_alive: convenience for `!person.is_dead`. Path: "is_alive".
    IS_ALIVE,
    // geo_unit.<LEVEL>: the person's ancestor geographical unit at a named
    // level, compared by unit name. Path: "geo_unit.XLGU".
    GEO_ANCESTOR,
  };

  // Ancestor-geography membership, one entry per geographical unit, built
  // once at resolve time so evaluate is a single array read: 0 = under some
  // other unit, 1 = under a target unit, 2 = no ancestor at this level
  // (absent — false whichever way the criterion is written).
  mutable std::vector<uint8_t> geo_ancestor_mask;
  // Empty when the mask is keyed by geo_unit_id directly (ids dense enough to
  // make that cheap). Otherwise holds the world's unit ids in sorted order and
  // the mask is keyed by position in it, found by binary search. Both forms are
  // self-contained: evaluate answers from the criterion alone, never from a
  // WorldState it may not have been handed.
  mutable std::vector<GeoUnitId> geo_mask_unit_ids;
  // Recorded rather than thrown: evaluate resolves lazily and must not throw
  // from the hot path, so resolveOrThrow is what turns this into an error.
  mutable std::string geo_resolve_error;

  void buildGeoAncestorMask(const WorldState& world) const;
  // Position of `id` in geo_ancestor_mask, or geo_ancestor_mask.size() when the
  // mask has no entry for it.
  size_t geoMaskSlot(GeoUnitId id) const;
  mutable PropertyType cached_type = PropertyType::UNKNOWN;
  mutable std::string cached_activity_name;  // (also reused for facet name)
  mutable std::string cached_sub_property;   // (also reused for facet field)
  mutable int cached_prop_idx = -1;
  mutable int32_t target_code = -1;  // Interned code for comparison
};

// =============================================================================
// CSV-based schedule assignment
// =============================================================================

/// One row from the schedule assignment CSV.
/// Rows are evaluated top-to-bottom; the first matching row wins.
struct ScheduleAssignmentRow {
  /// Criteria from filter.* columns. Empty = catch-all (matches everyone).
  std::vector<SelectionCriterion> criteria;

  /// (schedule_type_index, cumulative_upper_bound) sorted ascending.
  /// Built from the schedule.* column values after normalization.
  /// tryCSVAssignment draws a random value and finds the first entry whose
  /// cumulative bound exceeds the draw. Values not covered → YAML fallback.
  std::vector<std::pair<int, double>> schedule_probs;

  /// Probability mass reserved for YAML fallback (1.0 - sum of probs).
  /// 0.0 when probabilities were normalised to sum exactly to 1.0.
  double fallback_prob = 0.0;
};

// =============================================================================
// Time and Schedule Configuration
// =============================================================================

// Specifies a particular activity and which index to use (for people with
// multiple venues)
struct SpecifiedActivity {
  std::string type;  // e.g., "primary_activity", "leisure", etc.
  std::optional<std::string>
      venue_type;  // Optional: filter by venue type (e.g., "pub", "gym")
  int index =
      0;  // Which index from person's activity list to use (after filtering)
  // Pre-resolved
  int16_t cached_activity_idx =
      -1;  // Resolved index of 'type' in activity_names
  int cached_venue_type_idx =
      -1;  // Resolved index of venue_type in venue_type_names
};

struct TimeSlot {
  std::string name;
  std::string start;  // "HH:MM" format
  std::string end;    // "HH:MM" format
  std::vector<std::string>
      allowed_activities;  // List of valid activities for this slot

  // Activities available ONLY through coordinated encounters, not regular
  // activity assignment. E.g., "sex" can only happen via the coordinated
  // encounter pipeline during this slot, never through normal scheduling.
  std::vector<std::string> coordinated_only_activities;

  // Pre-resolved: bitmask of allowed activity indices (bit i =
  // activity_names[i] is allowed)
  ActivityMask allowed_activity_mask = 0;
  // Pre-resolved: vector of allowed activity indices
  std::vector<int16_t> allowed_activity_indices;

  // Pre-resolved: bitmask of coordinated-only activity indices.
  // Encounter trigger checks use (allowed_activity_mask |
  // coordinated_only_activity_mask). Regular ActivityManager uses only
  // allowed_activity_mask.
  ActivityMask coordinated_only_activity_mask = 0;

  // Optional: Specifies which index from person's activity list to use for a
  // specific activity type This allows control over activity selection (e.g.,
  // first job vs second job)
  std::optional<SpecifiedActivity> specified_activity;

  // Optional: per-activity schedule hop. YAML key: hop_on_activity: {act:
  // sched} If a person is assigned 'act' in this slot, they hop to 'sched'.
  std::unordered_map<std::string, std::string> hop_on_activity;
  // Pre-resolved: indexed by activity_idx; value = schedule_type index, or -1
  std::vector<int16_t> hop_schedule_by_activity_idx;

  // Optional: per-activity property-dispatched hop.
  // YAML key: hop_on_activity: {act: {property: <name>, schedule_template:
  // "<prefix>{value}<suffix>"}}. At runtime the person property named
  // 'property' is read as an integer; '{value}' in schedule_template is
  // replaced with it to form the target schedule name.
  struct PropertyDispatchHop {
    std::string property_name;           // person property to read
    std::string schedule_name_template;  // "{value}" replaced by property int
  };
  std::unordered_map<std::string, PropertyDispatchHop> property_hop_dispatch;
  // Pre-resolved: activity_idx → dispatch rule
  std::unordered_map<int16_t, PropertyDispatchHop>
      property_hop_dispatch_by_activity_idx;
};

struct ScheduleType {
  std::string name;
  int priority = 0;  // Higher priority checked first

  // If true, person auto-returns after executing all flat_slots
  bool is_temporary = false;
  // Ordered sequence of slots for temporary schedules (executed slot-by-slot)
  std::vector<TimeSlot> flat_slots;
  // Optional schedule to return to after a temporary sequence completes.
  // Empty = return to person's original schedule_type_id.
  std::string return_schedule;
  int16_t return_schedule_idx = -1;  // pre-resolved; -1 = use original

  // Selection criteria - ALL must match for this schedule type to apply
  std::vector<SelectionCriterion> selection_criteria;

  // Time slots per day type: day_type_name -> slot list
  std::unordered_map<std::string, std::vector<TimeSlot>> slots_by_day_type;

  // Participation rates: day_type_name -> activity_name -> rate
  std::unordered_map<std::string, std::unordered_map<std::string, double>>
      participation_by_day_type;

  // Pre-resolved: participation_by_day_type_id[day_type_idx][activity_idx]
  std::vector<std::vector<double>> participation_by_day_type_id;

  // Pre-resolved: slots_by_day_type_idx[day_type_idx] (nullptr if not defined)
  std::vector<const std::vector<TimeSlot>*> slots_by_day_type_idx;

  // Per-schedule override: activities listed here are demoted from
  // DETERMINISTIC to HYBRID at precompute time. Venue is still pre-cached
  // (no perf loss on venue lookup) but participation is re-rolled each tick
  // via the existing hybrid-entry runtime path.
  std::vector<std::string> force_hybrid_activities;
  ActivityMask force_hybrid_mask = 0;  // resolved in resolveSlots()

  // Linked activities: activities listed here share ONE dice roll per
  // (person, sim_day). All listed activities pass or fail together. The
  // engine caches the first roll on Person.linked_activities_pass and reuses
  // it for the rest of the day. Implies force_hybrid (these activities must
  // be re-rolled at runtime, not frozen at precompute), so listing an
  // activity here automatically adds it to force_hybrid_mask too.
  // The participation rate used is the rate of the first listed activity in
  // the schedule's `participation` table. For the coupling to make semantic
  // sense all listed activities should have the same rate.
  std::vector<std::string> linked_activities;
  ActivityMask linked_activities_mask = 0;  // resolved in resolveSlots()

  // Check if this schedule type applies to a person
  bool appliesTo(const Person& person,
                 const WorldState* world = nullptr) const {
    // Empty criteria = matches everyone (fallback schedule)
    if (selection_criteria.empty()) {
      return true;
    }

    // All criteria must match
    for (const auto& criterion : selection_criteria) {
      if (!criterion.evaluate(person, world)) {
        return false;
      }
    }
    return true;
  }

  void resolve(const WorldState& world) {
    for (auto& criterion : selection_criteria) {
      criterion.resolveOrThrow(world,
                               "schedule type '" + name + "' selection");
    }
    // force_hybrid_mask is resolved in ScheduleConfig::resolveSlots (defined
    // in config.cpp where WorldState is complete).
  }
};

struct ScheduleConfig {
  // Ordered cycle of day type names. sim_day maps to cycle[sim_day % len].
  // For a standard Mon-Sun week starting Monday:
  //   [workday, workday, workday, workday, workday, rest_day, rest_day]
  std::vector<std::string> day_type_cycle;

  // Deduplicated unique day type names (order of first appearance in cycle)
  std::vector<std::string> day_type_names;

  // Pre-resolved: cycle_to_type_idx[cycle_pos] = index into day_type_names
  std::vector<int> cycle_to_type_idx;

  // Schedule types (sorted by priority, highest first)
  std::vector<ScheduleType> schedule_types;
  std::string default_schedule_type = "standard_worker";

  // Get day type index for a given simulation day
  int getDayTypeIndex(int sim_day) const {
    if (day_type_cycle.empty() || cycle_to_type_idx.empty()) return 0;
    int cycle_pos = sim_day % static_cast<int>(day_type_cycle.size());
    return cycle_to_type_idx[cycle_pos];
  }

  // Get schedule type for a person (checks criteria in priority order)
  const ScheduleType* getScheduleTypeForPerson(
      const Person& person, const WorldState* world = nullptr) const {
    // Check each schedule type in priority order; skip temporary schedules
    // (day_trip, long_trip, etc.), they are only used for schedule hopping,
    // never as a person's base schedule.
    for (const auto& sched_type : schedule_types) {
      if (sched_type.is_temporary) continue;
      if (sched_type.appliesTo(person, world)) {
        return &sched_type;
      }
    }

    // Fallback: use default schedule type if specified
    for (const auto& sched_type : schedule_types) {
      if (sched_type.name == default_schedule_type) {
        return &sched_type;
      }
    }

    // Ultimate fallback: return first non-temporary schedule type
    for (const auto& sched_type : schedule_types) {
      if (!sched_type.is_temporary) return &sched_type;
    }

    return nullptr;  // No schedule types defined
  }

  // Optional path to a CSV file that assigns schedules probabilistically.
  // Set from schedules.yaml "schedule_csv:" key. Empty = no CSV assignment.
  std::string csv_path;

  // Parsed CSV rows, populated by resolveCSV(world). Empty until resolved.
  std::vector<ScheduleAssignmentRow> csv_rows;

  /// Try to assign a schedule from CSV rows. Returns the chosen ScheduleType*
  /// on a match, or nullptr if no row matched or the random draw fell in the
  /// fallback range (caller should then use YAML selection_criteria).
  const ScheduleType* tryCSVAssignment(const Person& person,
                                       const WorldState& world,
                                       std::mt19937& rng) const;

  /// Parse csv_path (if set) and populate csv_rows. Resolves geo_unit names
  /// to sets of geo_unit_ids. Throws on unknown schedule names or bad probs.
  /// Called from Config::resolve(world) after the world is loaded.
  void resolveCSV(const WorldState& world);

  void resolve(const WorldState& world) {
    for (auto& sched_type : schedule_types) {
      sched_type.resolve(world);
    }
    resolveSlots(world);
  }

  // Resolve all TimeSlot caches (allowed_activity_mask, indices, etc.)
  void resolveSlots(const WorldState& world);
};

// =============================================================================
// Vaccination Configuration
// =============================================================================

struct DoseConfig {
  int number;
  double days_to_effective;
  double days_to_waning;
  double days_to_finished;
  double waning_factor = 1.0;

  // Disease Name -> list of age-stratified efficacies
  std::unordered_map<std::string, std::vector<AgeEfficacy>> infection_efficacy;
  std::unordered_map<std::string, std::vector<AgeEfficacy>> symptom_efficacy;
};

struct VaccineConfig {
  std::string name;
  std::vector<DoseConfig> doses;
};

struct VaccinationCampaignConfig {
  std::string name;
  std::string start_date;
  std::string end_date;

  std::string vaccine_type;
  std::vector<int> dose_sequence;
  std::vector<double> days_to_next_dose;  // Time between doses in sequence

  double daily_coverage = 0.0;

  // Selection criteria for eligibility
  std::vector<SelectionCriterion> selection_criteria;

  // Optional: filter by previous vaccine type (for boosters)
  std::vector<std::string> last_dose_type_filter;

  void resolve(const WorldState& world) {
    for (auto& crit : selection_criteria) {
      crit.resolveOrThrow(world,
                          "vaccination campaign '" + name + "' selection");
    }
  }
};

struct VaccinationConfig {
  bool enabled = false;
  std::unordered_map<std::string, VaccineConfig> vaccines;
  std::vector<VaccinationCampaignConfig> campaigns;

  void resolve(const WorldState& world) {
    if (!enabled) return;
    for (auto& campaign : campaigns) {
      campaign.resolve(world);
    }
  }
};

// =============================================================================
// Contact Matrix Configuration
// =============================================================================

struct ContactMatrix {
  // Contact matrix: from_bin -> to_bin -> number_of_contacts
  std::vector<std::vector<double>> contacts;

  // Physical contact proportion matrix
  std::vector<std::vector<double>> proportion_physical;

  // Bin names (e.g., ["residents", "workers"])
  std::vector<std::string> bins;

  // Characteristic time in hours
  double characteristic_time = 24.0;

  // Get number of contacts between two bins
  double getContacts(size_t from_bin, size_t to_bin) const {
    if (from_bin < contacts.size() && to_bin < contacts[from_bin].size()) {
      return contacts[from_bin][to_bin];
    }
    return 0.0;
  }

  // Get proportion physical between two bins
  double getProportionPhysical(size_t from_bin, size_t to_bin) const {
    if (from_bin < proportion_physical.size() &&
        to_bin < proportion_physical[from_bin].size()) {
      return proportion_physical[from_bin][to_bin];
    }
    return 0.0;
  }

  // Find bin index by name (kept for non-hot-path usage)
  int findBinIndex(const std::string& bin_name) const {
    for (size_t i = 0; i < bins.size(); ++i) {
      if (bins[i] == bin_name) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  // Pre-resolved: subset_type_id -> bin index (populated by
  // ContactMatrixConfig::resolve)
  std::vector<int> bin_by_subset_type;
  // Pre-resolved sex bins
  int male_bin = -1;
  int female_bin = -1;

  // Pre-computed age -> bin index lookup table (ages 0-99, -1 = no mapping)
  int age_to_bin[100] = {};
  // True if at least one bin name was parsed as an age range
  bool has_age_bins = false;

  // Resolve internal bin names
  void resolve(const WorldState& world) {}
};

struct ContactMatrixConfig {
  // Beta values (transmission coefficients) per venue type
  std::unordered_map<std::string, double> betas;

  // Contact matrices per venue type
  std::unordered_map<std::string, ContactMatrix> matrices;

  // Mapping from encounter_type_id -> virtual_contact_matrix name
  // Populated during CoordinatedEncounterConfig::resolve()
  std::unordered_map<uint8_t, std::string> virtual_matrix_names;

  // Encounter type ids that take place at a virtual venue, sorted. Only these
  // ever consult a virtual matrix: an encounter held at a physical venue mixes
  // its participants with everyone else there, under that venue's own matrix,
  // so it needs no matrix of its own. Populated during
  // CoordinatedEncounterConfig::resolve() for every is_virtual encounter,
  // including ones that named no matrix, since that omission is exactly what
  // load-time resolution should catch.
  std::vector<uint8_t> virtual_encounter_type_ids;

  // Deterministic name→id mapping (sorted map ensures consistent IDs across
  // ranks)
  std::map<std::string, int> matrix_name_to_id;

  // Default values
  double default_beta = 0.05;
  double default_proportion_physical = 0.1;
  double default_characteristic_time = 24.0;
  double alpha_physical = 1.0;

  // Global transmission scaling
  struct GlobalBetaConfig {
    bool enabled = false;
    double value = 1.0;
  } global_beta;

  // Ordered mode names. Single-mode configs use {"default"}.
  std::vector<std::string> mode_names;

  // default_matrix: stands in for (venue, mode) pairs with no entry of their
  // own, when default_contacts_matrix has no `modes:` block. Reaching it is
  // refused at load unless allow_default_matrix is set.
  std::optional<ContactMatrix> default_matrix;

  // default_mode_matrices[mode_name] → ContactMatrix: the per-mode form of
  // the above, used when default_contacts_matrix has a `modes:` block.
  std::optional<std::unordered_map<std::string, ContactMatrix>>
      default_mode_matrices;

  // mode_matrices[venue_type][mode_name] → ContactMatrix
  std::unordered_map<std::string,
                     std::unordered_map<std::string, ContactMatrix>>
      mode_matrices;

  // Get beta for a venue type
  double getBeta(const std::string& venue_type) const {
    auto it = betas.find(venue_type);
    if (it != betas.end()) {
      return it->second;
    }
    return default_beta;
  }

  // Get beta by venue type ID
  double getBeta(uint8_t venue_type_id) const {
    if (venue_type_id < betas_by_id.size()) {
      return betas_by_id[venue_type_id];
    }
    return default_beta;
  }

  // Get contact matrix for a venue type
  const ContactMatrix* getMatrix(const std::string& venue_type) const {
    auto it = matrices.find(venue_type);
    if (it != matrices.end()) {
      return &it->second;
    }
    return nullptr;
  }

  int numModes() const { return static_cast<int>(mode_names.size()); }

  /// The bin structure of a venue type: which strata its occupants are divided
  /// into for contact counting. Bins are a property of the venue type and are
  /// identical across transmission modes (finalizeResolvedMatrices refuses to
  /// load a scenario where they are not), so this is a mode-free question and
  /// callers that only need bin counts or bin assignment ask it here rather
  /// than picking an arbitrary mode's matrix.
  const ContactMatrix& getBinStructure(uint8_t venue_type_id) const {
    if (venue_type_id >= bin_structure_by_id.size() ||
        bin_structure_by_id[venue_type_id] == nullptr) {
      throwUnresolved("venue type id", venue_type_id);
    }
    return *bin_structure_by_id[venue_type_id];
  }

  /// The contact matrix for (venue type, transmission mode). Every pair the
  /// world can present was resolved at load, so this is a direct read that
  /// cannot come back empty; an out-of-range id or mode means the caller
  /// built one that no world registry produced.
  const ContactMatrix& getMatrix(uint8_t venue_type_id, int mode_index) const {
    if (venue_type_id >= resolved_by_id.size() || mode_index < 0 ||
        mode_index >= (int)resolved_by_id[venue_type_id].size() ||
        resolved_by_id[venue_type_id][mode_index] == nullptr) {
      throwUnresolved("venue type id", venue_type_id, mode_index);
    }
    return *resolved_by_id[venue_type_id][mode_index];
  }

  /// The bin structure of a virtual encounter type, the encounter-keyed
  /// counterpart of getBinStructure.
  ///
  /// Virtual encounters (group_sex, romantic_encounter, etc.) live under
  /// string keys in `matrices`/`mode_matrices` and are NOT venue types, so
  /// they are resolved into their own encounter-keyed table. Encounter ids
  /// and venue type ids are disjoint integer spaces: reading one with the
  /// other's id silently pulls whatever matrix happens to sit at that slot.
  const ContactMatrix& getVirtualBinStructure(uint8_t encounter_type_id) const {
    if (encounter_type_id >= virtual_bin_structure_by_id.size() ||
        virtual_bin_structure_by_id[encounter_type_id] == nullptr) {
      throwUnresolved("encounter type id", encounter_type_id);
    }
    return *virtual_bin_structure_by_id[encounter_type_id];
  }

  /// The contact matrix for (virtual encounter type, transmission mode).
  /// Resolved at load like the venue-keyed table, so this is a direct read.
  /// Only encounters held at a virtual venue appear here; one held at a
  /// physical venue mixes under that venue's matrix instead.
  const ContactMatrix& getVirtualMatrix(uint8_t encounter_type_id,
                                        int mode_index) const {
    if (encounter_type_id >= resolved_virtual_by_id.size() || mode_index < 0 ||
        mode_index >= (int)resolved_virtual_by_id[encounter_type_id].size() ||
        resolved_virtual_by_id[encounter_type_id][mode_index] == nullptr) {
      throwUnresolved("encounter type id", encounter_type_id, mode_index);
    }
    return *resolved_virtual_by_id[encounter_type_id][mode_index];
  }

  void resolve(const WorldState& world);

  /// Rebuilds default_mode_matrices_by_id keyed to the disease's own mode
  /// order, so the per-mode default is reachable regardless of what (or
  /// whether) contact_matrices.mode_names declared. Throws if a disease mode
  /// has neither a default_mode_matrices entry nor a flat default_matrix.
  void finalizeDefaultModeMatrices(
      const WorldState& world,
      const std::vector<std::string>& disease_mode_names);

  /// Resolves every (venue type, mode) and (encounter type, mode) pair the
  /// world can present into a concrete matrix, once, so the hot path is a
  /// direct read with no fallback chain to walk.
  ///
  /// Two things are enforced here rather than discovered at run time:
  /// a venue type's bins must agree across every mode (bins describe who the
  /// occupants are, which no transmission route can change), and any pair
  /// that could only be filled by borrowing the scenario default is reported
  /// by name and refused unless `allow_default_matrix` is set. Borrowing the
  /// default is not an error in itself; doing so unnoticed is, because the
  /// resulting physics is not what the scenario says about that venue type.
  ///
  /// Keyed to the world's global registries, never to which venues happen to
  /// live on this rank, so a scenario loads everywhere or fails everywhere.
  void finalizeResolvedMatrices(
      const WorldState& world,
      const std::vector<std::string>& disease_mode_names);

  /// Validates the disease's mode list against this config's own: duplicate
  /// disease mode names throw, and a contact_matrices.yaml mode that no
  /// disease mode claims is orphaned and warns via stderr. Matrices are
  /// matched to modes by name in finalizeResolvedMatrices, so nothing here
  /// depends on the two files' independently-derived list orders.
  void finalizeDiseaseModeAlignment(
      const std::vector<std::string>& disease_mode_names);

  /// Set by a scenario that deliberately wants venue types with no matrix of
  /// their own to borrow the scenario-wide default. Off by default, so
  /// defaulting is something a scenario states rather than something that
  /// happens to it.
  bool allow_default_matrix = false;

  /// Whether finalizeResolvedMatrices has run. Anything that computes
  /// transmission checks this up front: an unresolved lookup would otherwise
  /// throw partway through a slot, and under MPI a single rank throwing
  /// inside a collective hangs the whole job instead of failing it.
  bool isResolved() const { return resolved_finalized_; }

 private:
  bool resolved_finalized_ = false;
  [[noreturn]] void throwUnresolved(const char* what, int id,
                                    int mode_index = -1) const;

  // Built by finalizeResolvedMatrices and read by every hot-path lookup.
  // [venue_type_id][disease_mode_index] and [encounter_type_id][mode]; both
  // are fully populated for every id the world registered, so a null entry
  // means the id came from somewhere other than a world registry.
  std::vector<std::vector<const ContactMatrix*>> resolved_by_id;
  std::vector<std::vector<const ContactMatrix*>> resolved_virtual_by_id;
  // [id] → the matrix whose bins define that type's stratification. Points at
  // one of the resolved matrices; separate because bins are a mode-free
  // property and callers asking for them should not have to name a mode.
  std::vector<const ContactMatrix*> bin_structure_by_id;
  std::vector<const ContactMatrix*> virtual_bin_structure_by_id;

  std::vector<double> betas_by_id;
  // [disease_mode_index] → the default matrix for that mode, or nullptr.
  // Rebuilt against the disease's own mode list by finalizeDefaultModeMatrices
  // and read while resolving pairs that have no matrix of their own.
  std::vector<const ContactMatrix*> default_mode_matrices_by_id;
};

// =============================================================================
// Simulation Configuration
// =============================================================================

struct AgeGroup {
  std::string name;
  int min_age;
  int max_age;

  // Helper method to check if an age is in this group
  bool contains(int age) const { return age >= min_age && age <= max_age; }
};

struct SimulationConfig {
  // Time settings
  std::string start_date;  // "YYYY-MM-DD"
  std::string end_date;
  std::string disease_file;
  std::string contact_matrices_file;
  std::string schedules_file;
  std::string vaccines_file;
  std::string activity_preferences_file;
  std::string coordinated_encounters_file;
  std::string performance_file;
  std::string parallel_file;
  std::string policies_file;
  std::string infection_seeds_file;
  // Optional path to a compartmental model plugin sidecar YAML.
  // Empty = no plugin; simulation runs with zero overhead.
  std::string compartmental_model_sidecar;

  // Optional calendar event CSVs. Empty = no calendar events (no-op).
  std::string calendar_events_file;
  std::string calendar_event_catchment_rules_file;

  // Optional on-the-fly venue allocator config. Empty = allocator not created.
  std::string on_the_fly_venues_file;

  // Seed for global simulation (transmissions, participation)
  unsigned int random_seed = 0;

  // Verbose output (debug-level diagnostics at load time)
  bool verbose = false;

  // Output settings
  int stats_interval_days = 1;  // Output statistics every N days
  std::string save_full_person_details = "infected_only";
  std::string save_person_activities = "none";
  int compression_level = 6;
  int flush_interval_days = 0;  // 0 = only at the end
  int max_event_buffer_size = 100000;
  bool save_population_summary = true;
  bool save_coordinated_encounters = false;  // largest H5 dataset; opt-in
  std::vector<std::string> summary_properties = {
      "ethnicity", "has_comorbidities", "work_mode"};

  // Regional Risk Factors
  struct RegionalRiskConfig {
    bool enabled = false;
    std::string regional_risk_file = "";
  } regional_risk;

  // Checkpoint / restart. Cadence is MUTUALLY EXCLUSIVE: if on_dates is
  // present (non-null, non-empty) it takes precedence and every_n_days is
  // ignored. A null YAML value leaves the corresponding optional empty.
  struct CheckpointConfig {
    bool enabled = false;
    std::string output_dir = "checkpoints/";  // resolved under run dir
    std::optional<int> every_n_days;          // null => absent
    std::optional<std::vector<std::string>>
        on_dates;       // ISO YYYY-MM-DD; null => absent
    int keep_last = 0;  // 0 => keep all

    // True when date-based cadence is the active mode.
    bool usesDates() const {
      return on_dates.has_value() && !on_dates->empty();
    }

    // Decide whether to checkpoint at the end of a completed day.
    //   completed_day_index : 0-based index of the day just finished
    //   current_date_iso    : that day's date, "YYYY-MM-DD"
    bool triggersOnDay(int completed_day_index,
                       const std::string& current_date_iso) const {
      if (!enabled) return false;
      if (usesDates()) {
        for (const auto& d : *on_dates)
          if (d == current_date_iso) return true;
        return false;
      }
      if (every_n_days.has_value() && *every_n_days > 0)
        return ((completed_day_index + 1) % *every_n_days) == 0;
      return false;
    }
  } checkpoint;

  // Partial-presence venues: at each slot, the RuntimeGroupAllocator buckets
  // riders of these venue types into ephemeral runtime groups (e.g. carriages
  // of a train_line) and the FOI loop drives sub-interval transmission from
  // each rider's per-membership (t_board_min, t_alight_min).
  //
  // Number of bins is emergent, derived at slot time from the global rider
  // count and the per-venue-type `target_group_size`. There is no hard
  // capacity; groups differ in size by at most 1 (round-robin deal). Empty
  // map (default) = feature inactive, zero hot-path overhead.
  struct PartialPresenceConfig {
    // YAML-declared: type name → target group size.
    // Example: {"train_line": 100, "tube_line": 100, "bus_line": 50}.
    std::unordered_map<std::string, int> target_group_size_by_name;

    // Resolved at world-load time. Bit i set iff venue type id i is a
    // partial-presence type present in this world. uint64_t fits 64 venue
    // types; promote to bitset if a world ever exceeds that.
    uint64_t enabled_venue_type_mask = 0;

    // Resolved at world-load time. Indexed by venue type id; entries for
    // non-partial-presence types are 0. Lookup-by-id is the hot path.
    std::vector<int> target_group_size_by_type_id;

    int getTargetGroupSize(uint8_t type_id) const {
      return type_id < target_group_size_by_type_id.size()
                 ? target_group_size_by_type_id[type_id]
                 : 0;
    }
  } partial_presence;

  void resolve(const WorldState& world);
};

// =============================================================================
// Coordinated Encounters Configuration
// =============================================================================

// Distribution type for invite counts and daily budgets (uint8_t for fast
// comparison)
enum class DistributionType : uint8_t { POISSON = 0, BINOMIAL, FIXED };

DistributionType parseDistributionType(const std::string& s);
const char* distributionTypeToString(DistributionType t);
struct InviteDistribution {
  DistributionType type = DistributionType::FIXED;
  double mean = 1.0;  // For poisson: λ (expected number of invites)
  double p = 0.5;     // For binomial: per-friend invite probability
  int count = 1;      // For fixed: exact number of invites
};

struct CoordinatedEncounterDef {
  std::string name;
  std::string network;
  std::string network_partner_filter = "";  // Only invite partners with this
                                            // tie_tag (empty = no filter)
  std::vector<std::string> trigger_slots;   // e.g., ["leisure", "social"]
  std::vector<std::string> allowed_venues;  // e.g., ["pub", "restaurant"]

  // General Settings
  bool enabled = true;
  int priority = 0;  // Lower = higher precedence (processed first)

  // Per-type daily budget: max encounters of this type per person per day
  // Sampled from distribution, clamped to [0, available_trigger_slots]
  InviteDistribution daily_max_distribution;  // defaults: type="fixed", count=1

  // Probabilities
  double proposal_probability = 0.0;
  InviteDistribution invite_distribution;
  double acceptance_probability = 1.0;

  // Optional: name of the frequency group that supplies this encounter's
  // per-person daily proposal rate from an external CSV. When set, the
  // scalar proposal_probability is ignored, the host's daily roll uses
  // the rate looked up from the frequency group's table. Encounters sharing
  // a group share ONE roll per person per day (so their total realized rate
  // sums to the CSV value, regardless of partner-mix).
  std::optional<std::string> frequency_group;

  // Minimum attendees required at injection time (after policy filtering).
  // If fewer than this many participants survive policy checks, the encounter
  // is cancelled and all participants keep their original activity assignments.
  // Default 2: an encounter with yourself is not an encounter.
  int min_attendees = 2;

  // Virtual Settings
  bool is_virtual = false;
  std::string virtual_contact_matrix = "";

  // Pre-resolved caches (populated by CoordinatedEncounterConfig::resolve)
  ActivityMask trigger_mask = 0;        // Bitmask of trigger activity indices
  ActivityMask allowed_venue_mask = 0;  // Bitmask of allowed venue type indices
  int cached_network_idx = -1;          // Resolved network type index
  uint8_t cached_encounter_type_id =
      kDefaultEncounterTypeId;  // Resolved encounter type ID
  int cached_virtual_venue_type_id =
      kUnknownVenueTypeId;  // Resolved virtual venue type ID
};

// Raw row from a frequency-group CSV, resolved for fast per-person lookup.
struct FrequencyRow {
  std::vector<SelectionCriterion> criteria;  // matched against Person
  double daily_probability = 0.0;            // rate normalized to per-day
};

// One named frequency domain (e.g. "sexual", "social"). Encounters reference
// this by name via CoordinatedEncounterDef::frequency_group.
struct FrequencyGroup {
  std::string name;
  std::string csv_path;
  std::string rate_column;
  std::string rate_unit;  // "per_day" | "per_week" | "per_month" | "per_year"
  std::vector<FrequencyRow> rows;
};

// A follower is placed wherever a host ends up, every slot they are bound. The
// subsystem has three independent knobs: which pool a host's followers come
// from, how the binding forms (establishment), and how long it lasts (span).

struct FollowConfig {
  bool enabled = false;

  // A short label for this rule, unique within the follows list. It defaults to
  // the rule's position and names its checkpoint shard, so it must be a valid
  // HDF5 group token (non-empty, no '/').
  std::string name;

  // Pool. Set exactly one. Venue co-members must live with the host (a
  // household), so the pool is entirely on the host's rank and needs no
  // messaging. A network draws the host's partner list, whose members can live
  // in other domains, so enrolment and mirroring are routed across ranks.
  std::string pool_venue_type;  // co-members of the host's venue of this type
  std::string network;          // OR the host's partners in this network

  // Establishment: how a follower binds to a host. Stochastic rolls a per-host
  // probability, keyed so the outcome is the same at any rank count. Criteria
  // uses the two predicates below instead, with no randomness, so the binding
  // can be recomputed from scratch and never needs to be saved in a checkpoint.
  enum class Establishment { Stochastic, Criteria };
  Establishment establishment = Establishment::Stochastic;
  double probability = 1.0;  // stochastic only: per-host chance to enrol

  // Criteria only: who becomes a follower, and who may host them. Each list is
  // a conjunction, so every criterion must hold and an empty list matches
  // everyone. These are the same person criteria policies and seeds take, which
  // is why the engine knows nothing about age here: whether a follower is a
  // toddler, a nurse or anyone at all is a sentence the scenario writes.
  std::vector<SelectionCriterion> follower;
  std::vector<SelectionCriterion> host;

  // Span: how long a binding survives. Hop lasts one of the host's trips and
  // ends when the host returns. Standing is re-checked each day and lasts until
  // the follower or host stops qualifying (a death, say).
  enum class Span { Hop, Standing };
  Span span = Span::Hop;

  // Three ways to stop mirroring for a slot, combined with OR, each meaning
  // "the follower keeps its own schedule this slot".
  //
  // Two of them ask where the HOST is going. activity_exceptions lists host
  // activities: an infant does not trail a parent to work (primary_activity) or
  // onto a ward when the parent is a patient (medical_facility), while a parent
  // sent home sick (residence) is still followed. venue_exceptions lists host
  // venue types, the cut activity cannot make since one activity reaches many
  // venues: leisure reaches a cinema, a gym and a grocery, so "follow to the
  // cinema but not the gym" can only be said as a venue type.
  //
  // The third asks what the FOLLOWER would otherwise be doing.
  // follower_activity_exceptions lists the follower's own activities, and it is
  // how a follower with somewhere of its own to be keeps that appointment: a
  // school-age child follows a parent around, but when the child's own day says
  // school, school wins, whether the parent is at work, at home or out. Without
  // it the host's location always overwrites the follower's, so a child bound
  // to a parent who has no primary activity would silently never reach school.
  std::vector<std::string> activity_exceptions;
  std::vector<std::string> venue_exceptions;
  std::vector<std::string> follower_activity_exceptions;

  std::string encounter_type;  // tag stamped on a follower's mirrored location
  bool log = false;            // log each follow when it forms (can be many)

  // Filled in by CoordinatedEncounterConfig::resolve against the world:
  int pool_venue_type_id = -1;
  int network_idx = -1;
  uint8_t encounter_type_id = kDefaultEncounterTypeId;
  std::vector<int16_t> activity_exception_ids;
  std::vector<uint8_t> venue_exception_type_ids;
  std::vector<int16_t> follower_activity_exception_ids;

  bool usesNetwork() const { return !network.empty(); }
  bool usesCriteria() const { return establishment == Establishment::Criteria; }
};

struct CoordinatedEncounterConfig {
  bool enabled = false;
  bool log_commitments = false;
  std::vector<CoordinatedEncounterDef> encounters;
  std::unordered_map<std::string, FrequencyGroup> frequency_groups;
  // Follow rules, resolved in list order. A scenario writes either a single
  // `follow:` block (sugar for a one-element list) or a `follows:` sequence.
  std::vector<FollowConfig> follows;

  void resolve(WorldState& world, ContactMatrixConfig& contact_matrices);
};

// =============================================================================
// Social Preference Configuration
// =============================================================================

struct PreferenceProfile {
  std::string name;
  std::string
      activity;  // The activity name this profile applies to (e.g., "leisure")
  int priority = 0;

  // Selection criteria to match people
  std::vector<SelectionCriterion> selection_criteria;

  // Map of venue_type -> weight
  std::unordered_map<std::string, double> preference_weights;

  bool appliesTo(const Person& person, const std::string& activity_name,
                 const WorldState* world = nullptr) const {
    // Must match activity name
    if (!activity.empty() && activity != activity_name) return false;

    if (selection_criteria.empty()) return true;
    for (const auto& criterion : selection_criteria) {
      if (!criterion.evaluate(person, world)) return false;
    }
    return true;
  }

  void resolve(const WorldState& world);

  // ID-based lookup buffers
  int activity_id = -1;
  std::vector<double> weights_by_id;
};

struct ActivityPreferenceConfig {
  std::vector<PreferenceProfile> profiles;

  double getWeight(const Person& person, int activity_id, uint8_t venue_type_id,
                   const WorldState* world = nullptr) const {
    for (const auto& profile : profiles) {
      if (profile.activity_id != -1 && profile.activity_id != activity_id)
        continue;

      if (profile.appliesTo(
              person, "",
              world)) {  // Empty string skip activity check in profile
        if (venue_type_id < profile.weights_by_id.size()) {
          return profile.weights_by_id[venue_type_id];
        }
      }
    }
    return 1.0;
  }

  void resolve(const WorldState& world) {
    for (auto& profile : profiles) {
      profile.resolve(world);
    }
  }
};

// =============================================================================
// Performance Configuration
// =============================================================================

struct PerformanceConfig {
  // Schedule pre-computation settings
  bool precompute_schedules = true;

  // Activities to pre-compute (deterministic)
  // These will be computed once and cached (e.g., "residence", "work",
  // "school")
  std::vector<std::string> deterministic_activities;

  // Hybrid activities: Precompute venue, re-evaluate participation
  // Venue is cached, but participation is rolled fresh each time (e.g.,
  // "primary_activity")
  std::vector<std::string> hybrid_activities;

  // Activities to compute at runtime (stochastic)
  // These will be randomly selected each time slot (e.g., "leisure", "social")
  // If empty, all activities not in deterministic_activities are stochastic
  std::vector<std::string> stochastic_activities;

  // Active infection tracking
  bool track_active_infections_only = true;

  // Pre-resolved bitmasks (populated by resolve())
  ActivityMask deterministic_mask = 0;
  ActivityMask hybrid_mask = 0;
  ActivityMask stochastic_mask = 0;
  bool masks_resolved = false;

  // Helper: Check if activity is hybrid by index (fast path)
  bool isHybridIdx(int16_t activity_idx) const {
    if (masks_resolved && activity_idx >= 0 && activity_idx < 128) {
      return (hybrid_mask >> activity_idx) & 1;
    }
    return false;
  }

  // Helper: Check if activity is hybrid (string path, kept for compatibility)
  bool isHybrid(const std::string& activity) const {
    return std::find(hybrid_activities.begin(), hybrid_activities.end(),
                     activity) != hybrid_activities.end();
  }

  // Helper: Check if activity should be pre-computed by index (fast path)
  bool isDeterministicIdx(int16_t activity_idx,
                          const TimeSlot* slot = nullptr) const {
    if (masks_resolved && activity_idx >= 0 && activity_idx < 128) {
      if ((hybrid_mask >> activity_idx) & 1) return false;
      if (slot && slot->specified_activity.has_value()) {
        const auto& spec_act = slot->specified_activity.value();
        if (spec_act.cached_activity_idx == activity_idx &&
            spec_act.venue_type.has_value()) {
          return true;
        }
      }
      if (deterministic_mask != 0)
        return (deterministic_mask >> activity_idx) & 1;
      if (stochastic_mask != 0) return !((stochastic_mask >> activity_idx) & 1);
      return true;
    }
    return true;
  }

  // Helper: Check if activity should be pre-computed (string path, kept for
  // compatibility)
  bool isDeterministic(const std::string& activity,
                       const TimeSlot* slot = nullptr) const {
    if (isHybrid(activity)) return false;
    if (slot && slot->specified_activity.has_value()) {
      const auto& spec_act = slot->specified_activity.value();
      if (spec_act.type == activity && spec_act.venue_type.has_value()) {
        return true;
      }
    }
    if (!deterministic_activities.empty()) {
      return std::find(deterministic_activities.begin(),
                       deterministic_activities.end(),
                       activity) != deterministic_activities.end();
    }
    if (!stochastic_activities.empty()) {
      return std::find(stochastic_activities.begin(),
                       stochastic_activities.end(),
                       activity) == stochastic_activities.end();
    }
    return true;
  }

  void resolve(const WorldState& world);
};

// =============================================================================
// Output Configuration
// =============================================================================

struct OutputConfig {
  // HDF5 compression level (0-9, where 0=none, 6=balanced, 9=max)
  int compression_level = 6;

  // Person data saving modes: "all", "infected_only", "none"
  std::string save_full_person_details = "infected_only";

  // Population summary: minimal demographic data for all people (for
  // denominators)
  bool save_population_summary = true;

  // Person activities saving modes: "all", "infected_only", "none"
  std::string save_person_activities = "none";

  // Streaming settings
  int flush_interval_days = 0;
  int max_event_buffer_size = 100000;
};

// =============================================================================
// Parallel Execution Configuration
// =============================================================================

struct ParallelConfig {
  bool enabled = false;
  // Partitioning settings
  std::string partition_level = "MGU";  // Geographic level to partition on
  std::string centroids_file = "data/domain_decomposition/mgu_centroids.csv";
  std::string adjacency_file =
      "data/domain_decomposition/mgu_adjacency_graph.json";

  // METIS options
  double metis_imbalance_tolerance = 0.05;  // 5% imbalance allowed

  // Chunked loading settings (memory efficiency)
  size_t person_metadata_chunk_size =
      100000;  // Process person metadata in chunks of N people
  size_t geo_unit_chunk_size =
      500;  // Process domain loading in chunks of N geo_units (parallel)

  // Communication
  int buffer_size_mb = 256;

  // Output
  bool save_partition = true;
  std::string partition_file = "output/partition.json";
  bool report_load_balance = true;
  bool report_communication = false;
  int report_interval_days = 1;
};

// =============================================================================
// Complete Configuration
// =============================================================================

struct Config {
  SimulationConfig simulation;
  ScheduleConfig schedule;
  ContactMatrixConfig contact_matrices;
  ActivityPreferenceConfig activity_preferences;
  PerformanceConfig performance;
  OutputConfig output;
  ParallelConfig parallel;
  VaccinationConfig vaccination;
  CoordinatedEncounterConfig coordinated_encounters;

  void resolve(WorldState& world);
};

}  // namespace june
