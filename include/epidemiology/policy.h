#pragma once

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/types.h"
#include "core/world_state.h"
#include "utils/deterministic_rng.h"
#include "utils/random.h"

namespace june {

// Forward declarations
class Disease;

// =============================================================================
// Activity Exemption - Specific conditions to exempt an activity
// =============================================================================

struct ActivityExemption {
  std::string activity_name;
  int16_t activity_index = -1;
  std::vector<SelectionCriterion> criteria;

  bool appliesTo(const Person& person, const WorldState* world = nullptr,
                 const Person* partner = nullptr) const {
    for (const auto& criterion : criteria) {
      if (!criterion.evaluate(person, world, partner)) {
        return false;
      }
    }
    return true;
  }

  void resolve(const WorldState& world) {
    activity_index =
        static_cast<int16_t>(world.getActivityIndex(activity_name));
    for (auto& criterion : criteria) {
      criterion.resolveOrThrow(world, "policy exemption for activity '" +
                                          activity_name + "'");
    }
  }
};

// Direction of the scheduled-venue gate on a PolicyAction. The two directions
// are mutually exclusive, so one mask plus this flag covers both.
enum class VenueGateDirection : uint8_t {
  None = 0,        // no venue filter: override at any venue type
  RestrictTo = 1,  // override only at the listed venue types
  ExemptFrom = 2   // override everywhere except the listed venue types
};

// The type of the Venue a Person physically occupies for one slot: the key a
// venue-gated PolicyAction is decided on. Deliberately not the Pin Venue,
// which is only where a freeze_in_place override anchors the Person.
//
// Three states, not two. Absent means the Person occupies no Venue this slot
// (in transit, at a virtual encounter) and never passes the filter, in either
// direction. Known carries a resolved type. Deferred holds an unresolved
// VenueId so the world lookup happens only if a gated action is actually
// consulted — getOverride runs once per Person per slot, so an ungated run
// must pay nothing.
//
// A Venue that exists but cannot be typed on this rank is a defect, not a
// state: resolveAgainst throws rather than admitting kUnknownVenueTypeId,
// which is exactly the silent fail-open this type exists to remove.
enum class SlotVenueTypeState : uint8_t { Absent, Known, Deferred };

class SlotVenueType {
 public:
  // Occupies no Venue this slot.
  static SlotVenueType absent() {
    return SlotVenueType(SlotVenueTypeState::Absent, kUnknownVenueTypeId,
                         kInvalidVenueId);
  }

  // The caller already resolved the type. Handing over kUnknownVenueTypeId
  // while claiming to know it is the defect this mission exists to catch, so
  // it throws here, at the producer.
  static SlotVenueType known(uint8_t type_id) {
    if (type_id == kUnknownVenueTypeId) {
      throw std::runtime_error(
          "SlotVenueType::known: caller claims a known venue type but passed "
          "kUnknownVenueTypeId (255); the slot venue type is unresolvable.");
    }
    return SlotVenueType(SlotVenueTypeState::Known, type_id, kInvalidVenueId);
  }

  // Look the type up later, if anything asks. A negative id is absent.
  static SlotVenueType fromVenue(VenueId venue_id) {
    if (venue_id < 0) return absent();
    return SlotVenueType(SlotVenueTypeState::Deferred, kUnknownVenueTypeId,
                         venue_id);
  }

  // Collapses Deferred to Absent or Known. Throws naming the venue if the
  // world cannot type it. Absent and Known pass through unchanged.
  SlotVenueType resolveAgainst(const WorldState& world) const {
    if (state_ != SlotVenueTypeState::Deferred) return *this;
    const uint8_t type_id = world.getVenueTypeId(venue_id_);
    if (type_id == kUnknownVenueTypeId) {
      throw std::runtime_error(
          "SlotVenueType::resolveAgainst: venue " + std::to_string(venue_id_) +
          " exists but this rank cannot name its type; the slot venue type is "
          "unresolvable.");
    }
    return SlotVenueType(SlotVenueTypeState::Known, type_id, venue_id_);
  }

  bool isAbsent() const { return state_ == SlotVenueTypeState::Absent; }

  // Precondition: Known. A sentinel must never survive to the gate, so an
  // unresolved value is a programming error rather than a return value.
  uint8_t typeId() const {
    if (state_ != SlotVenueTypeState::Known) {
      throw std::runtime_error(
          "SlotVenueType::typeId: value is not Known; resolveAgainst() must "
          "run before the venue gate is consulted.");
    }
    return type_id_;
  }

 private:
  SlotVenueType(SlotVenueTypeState state, uint8_t type_id, VenueId venue_id)
      : venue_id_(venue_id), state_(state), type_id_(type_id) {}

  VenueId venue_id_;
  SlotVenueTypeState state_;
  uint8_t type_id_;
};

struct PolicyAction {
  // Activities to override (empty = override all activities with "*")
  std::unordered_set<std::string> override_activities;
  uint64_t override_activity_mask = 0;  // BITMASK: support up to 64 activities
  bool override_all = false;

  // Venue types the override is restricted to (empty = any venue type).
  // ANDed with the activity mask: one Activity reaches many Venue types, so
  // this is what lets pubs close while groceries stay open.
  std::unordered_set<std::string> override_venue_types;

  // Venue types the override is exempt from (empty = no exemption). The
  // inverse direction: close everything except the listed types. Mutually
  // exclusive with override_venue_types, so both share one mask.
  std::unordered_set<std::string> exempt_venue_types;

  uint64_t venue_gate_mask = 0;  // BITMASK: support up to 64 venue types
  VenueGateDirection venue_gate_direction = VenueGateDirection::None;

  // Non-empty when a venue gate is configured but has not resolved. Set before
  // resolution is attempted and cleared only on success, so a swallowed
  // resolve() failure leaves the gate poisoned rather than silently absent.
  // Holds the message the gate throws when consulted.
  std::string venue_gate_error;

  // Generic exemptions
  std::vector<ActivityExemption> exemptions;

  // What activity to do instead
  std::string replacement_activity;  // e.g., "residence", "medical_facility"
  int16_t replacement_activity_index = -1;

  // Optional: hop to a schedule instead of replacing the activity.
  // When set, getOverride triggers a schedule hop rather than calling
  // getReplacementLocation. Only effective for persons already on a hop.
  std::string replacement_schedule;
  int16_t replacement_schedule_idx = -1;

  // Compliance rate (0.0 = no one complies, 1.0 = everyone complies)
  double compliance_rate = 1.0;

  // Check if this action should override a given activity
  bool shouldOverride(const std::string& activity_name) const {
    if (override_all) return true;
    return override_activities.count(activity_name) > 0;
  }

  // Check by index
  bool shouldOverride(int16_t activity_index) const {
    if (override_all) return true;
    if (activity_index < 0 || activity_index >= 64) return false;
    return (override_activity_mask & (1ULL << activity_index));
  }

  // True when a venue filter is configured at all. Cheap guard so ungated
  // actions never pay for a venue-type lookup. A poisoned gate counts as
  // configured, so an unresolved one is still consulted and still throws.
  bool hasVenueGate() const {
    return venue_gate_direction != VenueGateDirection::None ||
           !venue_gate_error.empty();
  }

  // A configured-but-unresolved gate must never read as "this policy is not
  // here". Checked before the activity test, not just at the gate: a failed
  // resolve() also leaves override_activity_mask unbuilt, so the action would
  // otherwise fall out on shouldOverride and never reach the gate at all.
  void throwIfVenueGateUnresolved() const {
    if (!venue_gate_error.empty()) throw std::runtime_error(venue_gate_error);
  }

  // Check whether the type of the Venue the person physically occupies this
  // slot passes the gate. A Person occupying no Venue never passes, in either
  // direction: "not at a listed venue type" and "at no venue at all" are
  // different questions, and only the first is what exempt-from asks. An
  // unresolvable type never reaches here — SlotVenueType throws at the
  // producer instead.
  bool passesVenueGate(const SlotVenueType& slot_venue_type) const {
    if (!venue_gate_error.empty()) throw std::runtime_error(venue_gate_error);
    if (venue_gate_direction == VenueGateDirection::None) return true;
    if (slot_venue_type.isAbsent()) return false;
    const uint8_t type_id = slot_venue_type.typeId();  // throws if Deferred
    const bool is_listed =
        type_id < 64 && (venue_gate_mask & (1ULL << type_id));
    return venue_gate_direction == VenueGateDirection::RestrictTo ? is_listed
                                                                  : !is_listed;
  }

  // Check if this action has an exemption for a given activity
  bool isExempt(const Person& person, int16_t activity_index,
                const WorldState* world = nullptr,
                const Person* partner = nullptr) const {
    for (const auto& exemption : exemptions) {
      if (exemption.activity_index == activity_index) {
        if (exemption.appliesTo(person, world, partner)) {
          return true;
        }
      }
    }
    return false;
  }

  // Resolve venue type names to a bitmask. Unlike SimulationConfig::resolve,
  // which silently ignores unknown venue types, an unknown name here throws: a
  // misspelt type would otherwise mean "nobody qualifies" and the policy would
  // quietly do nothing. Consequence: a policies.yaml naming 'pub' hard-fails on
  // a pub-less world.
  static uint64_t resolveVenueTypeMask(
      const WorldState& world,
      const std::unordered_set<std::string>& venue_type_names,
      const std::string& field_name, const std::string& policy_name) {
    uint64_t mask = 0;
    for (const auto& venue_type : venue_type_names) {
      int index = world.getVenueTypeIndex(venue_type);
      if (index < 0) {
        throw std::runtime_error("PolicyAction::resolve: policy '" +
                                 policy_name + "' lists unknown venue type '" +
                                 venue_type + "' in " + field_name + ".");
      }
      if (index >= 64) {
        throw std::runtime_error(
            "PolicyAction::resolve: policy '" + policy_name + "' venue type id " +
            std::to_string(index) + " ('" + venue_type + "') in " + field_name +
            " exceeds 64-bit mask width; promote venue_gate_mask to a wider "
            "bitset.");
      }
      mask |= (1ULL << index);
    }
    return mask;
  }

  void resolve(const WorldState& world, const std::string& policy_name = "") {
    venue_gate_mask = 0;
    venue_gate_direction = VenueGateDirection::None;
    // Poison the gate before attempting to resolve it. resolveVenueTypeMask
    // throws on a misspelt venue type precisely so a policy cannot look
    // configured yet never fire, and that guarantee must not rest on no caller
    // anywhere ever wrapping resolve() in a catch. A swallowed failure now
    // leaves the gate configured-but-unresolved, which throws when consulted
    // rather than degrading to the silently ungated state.
    venue_gate_error =
        (override_venue_types.empty() && exempt_venue_types.empty())
            ? ""
            : "PolicyAction: policy '" + policy_name +
                  "' has a venue gate that never resolved; a resolve() failure "
                  "was swallowed by a caller.";
    if (!override_venue_types.empty() && !exempt_venue_types.empty()) {
      throw std::runtime_error(
          "PolicyAction::resolve: policy '" + policy_name +
          "' sets both override_venue_types and exempt_venue_types; the two "
          "directions are mutually exclusive.");
    }
    if (!override_venue_types.empty()) {
      venue_gate_mask = resolveVenueTypeMask(world, override_venue_types,
                                             "override_venue_types",
                                             policy_name);
      venue_gate_direction = VenueGateDirection::RestrictTo;
    } else if (!exempt_venue_types.empty()) {
      venue_gate_mask = resolveVenueTypeMask(world, exempt_venue_types,
                                             "exempt_venue_types", policy_name);
      venue_gate_direction = VenueGateDirection::ExemptFrom;
    }
    venue_gate_error.clear();  // resolved: only reachable if nothing threw

    if (override_activities.empty() || override_activities.count("*") > 0) {
      override_all = true;
    } else {
      override_activity_mask = 0;
      for (const auto& act : override_activities) {
        int index = world.getActivityIndex(act);
        if (index >= 0 && index < 64) {
          override_activity_mask |= (1ULL << index);
        } else {
          std::cerr << "  [Policy Warning] Activity '" << act
                    << "' not found or index out of range for bitmask."
                    << std::endl;
        }
      }
    }

    // Resolve exemptions
    for (auto& exemption : exemptions) {
      exemption.resolve(world);
    }

    replacement_activity_index =
        static_cast<int16_t>(world.getActivityIndex(replacement_activity));

    if (!replacement_schedule.empty()) {
      replacement_schedule_idx = static_cast<int16_t>(
          world.getScheduleTypeIndex(replacement_schedule));
      if (replacement_schedule_idx < 0) {
        std::cerr << "  [Policy Warning] replacement_schedule '"
                  << replacement_schedule << "' not found." << std::endl;
      }
    }
  }
};

// =============================================================================
// ActiveWindow - the span of simulation time a policy is in force for
// =============================================================================

// Half-open [start_time, end_time): end_time is the first moment the policy is
// NOT in force. This mirrors the simulation's own window (`day < total_days_`,
// simulator.cpp), so an end_date of 2020-03-12 means "through 11 March" and
// adjacent windows abut exactly, with no slot where both are active.
struct ActiveWindow {
  double start_time = 0.0;  // days from simulation start
  double end_time = -1.0;   // -1 = no end

  bool contains(double current_time) const {
    if (current_time < start_time) return false;
    if (end_time != -1.0 && current_time >= end_time) return false;
    return true;
  }
};

// =============================================================================
// Symptom-Based Policy - Override behavior based on disease symptoms
// =============================================================================

struct SymptomPolicy {
  std::string name;

  // Span of simulation time this policy is in force for
  ActiveWindow window;

  // Symptoms that trigger this policy
  std::vector<std::string> trigger_symptoms;
  uint32_t trigger_symptom_mask = 0;  // BITMASK: support up to 32 symptoms

  // Link to another policy to follow up if this one ends
  std::string follow_up_policy_name;
  int16_t follow_up_policy_index = -1;

  // Behavioral inheritance
  bool inherit_compliance = true;
  bool inherit_refusal = false;

  // What to do
  PolicyAction action;

  // Optional: selection criteria (only apply to certain people)
  std::vector<SelectionCriterion> applies_to;

  // Check if this policy applies to a person's current symptom
  bool triggeredBy(const std::string& symptom) const {
    return std::find(trigger_symptoms.begin(), trigger_symptoms.end(),
                     symptom) != trigger_symptoms.end();
  }

  // Check by symptom ID
  bool triggeredBy(uint16_t symptom_id) const {
    if (symptom_id >= 32) return false;
    return (trigger_symptom_mask & (1u << symptom_id));
  }

  // Check if policy applies to this person (based on selection criteria)
  bool appliesTo(const Person& person,
                 const WorldState* world = nullptr) const {
    // Empty criteria = applies to everyone
    if (applies_to.empty()) {
      return true;
    }

    // All criteria must match
    for (const auto& criterion : applies_to) {
      if (!criterion.evaluate(person, world)) {
        return false;
      }
    }
    return true;
  }

  void resolve(const WorldState& world, const Disease& disease) {
    for (auto& crit : applies_to) {
      crit.resolveOrThrow(world, "symptom policy '" + name + "' applies_to");
    }

    // Intern action
    action.resolve(world, name);

    // Intern symptoms
    trigger_symptom_mask = 0;
    for (const auto& sym : trigger_symptoms) {
      uint16_t id = disease.getSymptomId(sym);
      if (id < 32) {
        trigger_symptom_mask |= (1u << id);
      }
    }
  }
};

// =============================================================================
// Temporal Policy - Override behavior during a time period (lockdowns, etc.)
// =============================================================================

struct TemporalPolicy {
  std::string name;

  // Span of simulation time this policy is in force for
  ActiveWindow window;

  // What to do
  PolicyAction action;

  // Optional: selection criteria (only apply to certain people)
  std::vector<SelectionCriterion> applies_to;

  // Check if policy applies to this person (based on selection criteria)
  bool appliesTo(const Person& person,
                 const WorldState* world = nullptr) const {
    // Empty criteria = applies to everyone
    if (applies_to.empty()) {
      return true;
    }

    // All criteria must match
    for (const auto& criterion : applies_to) {
      if (!criterion.evaluate(person, world)) {
        return false;
      }
    }
    return true;
  }

  void resolve(const WorldState& world) {
    for (auto& crit : applies_to) {
      crit.resolveOrThrow(world, "temporal policy '" + name + "' applies_to");
    }
    action.resolve(world, name);
  }
};

// =============================================================================
// FrozenPersonState - Sparse storage for persons frozen by a policy hop
// =============================================================================

struct FrozenPersonState {
  uint8_t triggering_policy_index;
  int16_t paused_hopped_schedule_id;  // travel schedule to resume on recovery
  int16_t paused_return_schedule_id;  // saved schedule_hop.return_schedule_id
  VenueId pin_venue_id;
  SubsetIndex pin_subset_index;
  // schedule_hop.temp_slot_progress NOT saved: preserved automatically
  // (non-temporary hops never touch it, so it holds the correct
  // travel-schedule resume position)
};

// =============================================================================
// PolicyManager - Manages all policies and determines activity overrides
// =============================================================================

class PolicyManager {
 public:
  // Width of Person::applicable_symptom_policy_mask / _temporal_policy_mask.
  static constexpr size_t kMaxPoliciesPerKind = 32;

  PolicyManager(WorldState& world);

  // Set base seed for deterministic RNG (MPI reproducibility)
  void setBaseSeed(uint64_t seed) { base_seed_ = seed; }

  // --- Checkpoint serialization ---
  // frozen_states_ pins the small set of persons mid policy-hop. It must be
  // saved/restored so an interrupted hop resumes correctly on restart.
  const std::unordered_map<PersonId, FrozenPersonState>& getFrozenStates()
      const {
    return frozen_states_;
  }
  void setFrozenStates(
      const std::unordered_map<PersonId, FrozenPersonState>& s) {
    frozen_states_ = s;
  }

  // Register policies
  void addSymptomPolicy(const SymptomPolicy& policy);
  void addTemporalPolicy(const TemporalPolicy& policy);

  // Get all policies (for inspection/debugging)
  const std::vector<SymptomPolicy>& getSymptomPolicies() const {
    return symptom_policies_;
  }
  const std::vector<TemporalPolicy>& getTemporalPolicies() const {
    return temporal_policies_;
  }

  // Main function: Check if a person's scheduled activity should be overridden
  // Returns std::nullopt if no override applies, otherwise returns the override
  // location.
  //
  // pin_venue_id/pin_subset_index are the Pin Venue: where a freeze_in_place
  // override anchors the person. slot_venue_type is the Slot Venue Type: the
  // key a venue-gated action is decided on. The two are separate arguments
  // because callers legitimately differ on them — a traveller in transit is
  // pinned at their last overnight venue while occupying no venue at all.
  std::optional<PersonLocation> getOverride(Person& person,
                                            int16_t scheduled_activity_index,
                                            VenueId pin_venue_id,
                                            SubsetIndex pin_subset_index,
                                            SlotVenueType slot_venue_type,
                                            double current_time,
                                            int time_slot_index,
                                            const Person* partner = nullptr);

  // Would a policy remove this Person from `activity_index` this slot? A
  // question, not an instruction: no freeze is established or released, no
  // schedule hop swapped, no compliance decision latched. There is deliberately
  // no Pin Venue argument, because nothing is pinned.
  //
  // Asked by the subsystems that only need the verdict — coordinated-encounter
  // eligibility and follow mirroring — so that reading the answer cannot commit
  // the consequence of it.
  bool suppressesParticipation(const Person& person, int16_t activity_index,
                               const SlotVenueType& slot_venue_type,
                               double current_time,
                               const Person* partner = nullptr) const;

  // End a freeze, but only if this policy is the one that established it —
  // another policy's freeze is not this policy's to lift.
  void releaseFreeze(Person& person, size_t policy_index) {
    auto frozen_it = frozen_states_.find(person.id);
    if (frozen_it == frozen_states_.end()) return;
    if (frozen_it->second.triggering_policy_index !=
        static_cast<uint8_t>(policy_index)) {
      return;
    }
    releaseAnyFreeze(person);
  }

  // End a freeze whichever policy established it, putting the person back on
  // the travel schedule they were pinned off. A person nobody froze is left
  // alone. The only place frozen_states_ entries are erased.
  void releaseAnyFreeze(Person& person) {
    auto frozen_it = frozen_states_.find(person.id);
    if (frozen_it == frozen_states_.end()) return;
    person.schedule_hop.restoreTargets(
        frozen_it->second.paused_hopped_schedule_id,
        frozen_it->second.paused_return_schedule_id);
    frozen_states_.erase(frozen_it);
  }

  // Clear all policies
  void clear() {
    symptom_policies_.clear();
    temporal_policies_.clear();
  }

  // Statistics
  size_t getSymptomPolicyCount() const { return symptom_policies_.size(); }
  size_t getTemporalPolicyCount() const { return temporal_policies_.size(); }

  // Precompute which policies can apply to each person (based on selection
  // criteria) This caches the results in person.applicable_*_policy_mask
  void precomputePolicyApplicability(std::vector<Person>& people);

  // Resolve all policy criteria and intern activities/symptoms
  void resolveAll(const Disease& disease) {
    rejectPoliciesBeyondMaskWidth(symptom_policies_, "symptom");
    rejectPoliciesBeyondMaskWidth(temporal_policies_, "temporal");

    for (auto& p : symptom_policies_) p.resolve(world_, disease);
    for (auto& p : temporal_policies_) p.resolve(world_);

    // Resolve follow-up policy indices
    for (auto& p : symptom_policies_) {
      if (!p.follow_up_policy_name.empty()) {
        for (size_t i = 0; i < symptom_policies_.size(); ++i) {
          if (symptom_policies_[i].name == p.follow_up_policy_name) {
            p.follow_up_policy_index = static_cast<int16_t>(i);
            break;
          }
        }
      }
    }
  }

 private:
  WorldState& world_;
  uint64_t base_seed_ = 0;

  std::vector<SymptomPolicy> symptom_policies_;
  std::vector<TemporalPolicy> temporal_policies_;

  // Sparse map: persons currently frozen by a policy-triggered schedule hop.
  // Only populated for the small minority of persons who are both travelling
  // and sick simultaneously. Avoids touching the Person struct.
  std::unordered_map<PersonId, FrozenPersonState> frozen_states_;

  // Cached residence activity index (resolved on first use)
  int16_t residence_act_idx_ = -1;
  void ensureResidenceIndexCached() {
    if (residence_act_idx_ < 0) {
      residence_act_idx_ =
          static_cast<int16_t>(world_.getActivityIndex("residence"));
    }
  }

  // Applicability is carried in a 32-bit mask per person, so a policy past the
  // 32nd can never fire. Name the first such policy rather than drop it.
  template <typename PolicyVector>
  static void rejectPoliciesBeyondMaskWidth(const PolicyVector& policies,
                                            const std::string& kind) {
    if (policies.size() <= kMaxPoliciesPerKind) return;
    throw std::runtime_error(
        "PolicyManager: " + std::to_string(policies.size()) + " " + kind +
        " policies exceeds the limit of " +
        std::to_string(kMaxPoliciesPerKind) + "; '" +
        policies[kMaxPoliciesPerKind].name + "' would never take effect");
  }

  // Helper: Apply compliance rate (returns true if person complies).
  // Const because it is a pure draw: SplitMix64 over (base_seed_, person_id,
  // policy_index), no stored RNG state, so the same triple always answers the
  // same way. That is what lets suppressesParticipation reach a verdict for an
  // undecided Person without latching the decision.
  bool checkCompliance(double compliance_rate, PersonId person_id,
                       uint32_t policy_index) const;

  // Is this Person taking part in policy `policy_bit`, without latching the
  // answer? getOverride commits the sticky-compliance decision the first time
  // it asks; this reads a decision already made, and falls back to redrawing
  // it when none has been. The redraw is safe because checkCompliance is pure:
  // it returns exactly what getOverride would latch. Reachable only if a query
  // beats ActivityManager to a Person in a slot, which the timeslot ordering
  // (activity assignment, then encounters, then follows) does not do.
  bool isParticipating(uint32_t decisions_mask, uint32_t participation_mask,
                       size_t policy_bit, double compliance_rate,
                       PersonId person_id, uint32_t compliance_index) const {
    if (decisions_mask & (1u << policy_bit)) {
      return participation_mask & (1u << policy_bit);
    }
    return checkCompliance(compliance_rate, person_id, compliance_index);
  }

  // The part of a policy decision that is a pure question: does this action
  // reach this Person's activity, at this slot's venue type, unexempted?
  // Shared by getOverride and suppressesParticipation so the gate ordering and
  // the unresolved-gate throw exist in exactly one place.
  //
  // Takes a resolver rather than a resolved SlotVenueType so an ungated action
  // never pays for the venue-type lookup — and never throws on a venue this
  // rank cannot type when no policy was asking about venues in the first place.
  template <typename SlotVenueTypeResolver>
  bool actionApplies(const PolicyAction& action, const Person& person,
                     int16_t activity_index,
                     SlotVenueTypeResolver&& resolve_slot_venue_type,
                     const Person* partner) const {
    action.throwIfVenueGateUnresolved();

    if (!action.shouldOverride(activity_index)) return false;

    if (action.hasVenueGate() &&
        !action.passesVenueGate(resolve_slot_venue_type())) {
      return false;
    }

    if (action.isExempt(person, activity_index, &world_, partner)) return false;

    return true;
  }

  // Helper: Get replacement location for a given activity name
  std::optional<PersonLocation> getReplacementLocation(
      const Person& person, const std::string& replacement_activity,
      int16_t replacement_activity_index = -1);
};

}  // namespace june
