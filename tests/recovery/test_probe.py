import copy
import base64
import json
import subprocess
import unittest
from itertools import product
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from probe import (
    ObservationProtocol, _action_payload, _calibration_payload, _contract_errors,
    canonical_sha256, evaluate_run,
)


STATUSES = [
    "PASS", "FAIL_PRODUCT", "INVALID_OBSERVATION", "BLOCKED_IDENTITY",
    "BLOCKED_ENVIRONMENT", "BLOCKED_INPUT_ORIGIN", "NOT_RUN_AUTHORIZATION",
    "ABORTED_SAFETY",
]
TEST_COLLECTOR_PRIVATE_KEY = Ed25519PrivateKey.from_private_bytes(bytes(range(1, 33)))
TEST_COLLECTOR_PUBLIC_KEY = base64.b64encode(TEST_COLLECTOR_PRIVATE_KEY.public_key().public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw
)).decode("ascii")
R10_AUTHORIZED_REPETITIONS_SHA256 = "984d30a8cf6acf4a6bb91042d979c2da0fb0ee169aa64222b92b54127d9da3cb"
R10_AUTHORIZED_CASES = 52
R10_PRIMARY_ATTEMPTS = 620


def base_case(case_id="manual"):
    value = {
        "id": case_id,
        "capability": "P0_MANUAL_WORD",
        "surface": "gtk_entry",
        "direction": "en_ru",
        "steps": ["physically_type:ghbdtn", "physically_press:Pause"],
        "initial_state": {
            "text": "ghbdtn",
            "selection": [6, 6],
            "focus_target": "gtk_entry",
            "layout": {"gnome_source": "en", "xkb_group": 0, "indicator": "en"},
        },
        "expected_state": {
            "text": "привет",
            "selection": [6, 6],
            "focus_target": "gtk_entry",
            "layout": {"gnome_source": "ru", "xkb_group": 1, "indicator": "ru"},
        },
        "next_input": {
            "symbol": "ф",
            "expected_state": {
                "text": "приветф",
                "selection": [7, 7],
                "focus_target": "gtk_entry",
                "layout": {"gnome_source": "ru", "xkb_group": 1, "indicator": "ru"},
            },
        },
        "repetitions": 1,
        "requires_physical_input": True,
    }
    value["checkpoints"] = [
        {
            "id": "after-operation",
            "after_action": "physically_press:Pause",
            "action_index": 1,
            "observer_id": "gtk",
            "time_constraint": {"within_attempt": True},
            "expected_state": copy.deepcopy(value["expected_state"]),
        },
        {
            "id": "after-next-input",
            "after_action": "physically_type:ф",
            "action_index": 2,
            "observer_id": "gtk",
            "time_constraint": {"within_attempt": True},
            "expected_state": copy.deepcopy(value["next_input"]["expected_state"]),
        },
    ]
    return value


def contract():
    return {
        "contract_id": "contract-r8",
        "task_id": "c2c_bfdc",
        "iteration": 7,
        "plan_revision": 8,
        "collector_attestation": {
            "algorithm": "ed25519",
            "public_key_base64": TEST_COLLECTOR_PUBLIC_KEY,
            "key_id": "synthetic-test-collector",
        },
        "statuses": STATUSES,
        "supported_environment": {
            "expected": {"session_type": "x11"},
            "required_present": ["seat"],
        },
        "artifacts": {
            "candidate": {
                "provenance_id": "candidate-provenance",
                "provenance": {"package_sha256": "a" * 64, "payload_sha256": "b" * 64},
            }
        },
        "budgets": {
            "oracle_seconds": 5,
            "diagnostic_replays_per_first_failure": 1,
            "soak": {
                "elapsed_seconds": 1800,
                "active_seconds": 1200,
                "idle_intervals_seconds": [300, 300],
                "operations": 3,
                "process_restarts": 0,
                "operation_case_cycle": ["manual"],
            },
        },
        "safety": {"abort_on": ["lost_text", "working_session_impact"]},
        "cases": [base_case()],
    }


def environment(**overrides):
    details = {"session_type": "x11", "seat": "seat0"}
    value = {
        "isolation": {"confirmed": True, "affects_working_session": False},
        "details": details,
        "fingerprint_sha256": canonical_sha256(details),
        "input_origin": {"kind": "physical", "evidence": "isolated-seat-capture"},
    }
    value.update(overrides)
    return value


def artifact(**overrides):
    value = {
        "id": "candidate",
        "provenance_id": "candidate-provenance",
        "provenance": {"package_sha256": "a" * 64, "payload_sha256": "b" * 64},
    }
    value.update(overrides)
    return value


NS = 1_000_000_000
GRAPH_NODE_IDS = [
    "freeze_and_reframe",
    "authoritative_behavior_contract",
    "baseline_2_8_5_probe",
    "candidate_2_8_14_probe",
    "architecture_route_decision",
    "manual_word_vertical_slice",
    "automatic_vertical_slice",
    "remaining_capabilities",
    "source_and_architecture_gate",
    "reversible_candidate_install",
    "real_target_acceptance_and_soak",
    "release_review_and_publication",
]


def observer_id_for(surface):
    return "chromium" if str(surface).startswith("chromium") else "gtk"


def observer_instance_for(observer_id):
    return f"{observer_id}-instance-1"


def clock_mapping(start, finish, observer_ids=None):
    observer_ids = tuple(observer_ids or ("gtk", "chromium"))
    return {
        "collector_domain": "monotonic_ns",
        "observers": {
            observer_instance_for(observer_id): {
                "mapping_id": f"{observer_id}-mapping",
                "observer_id": observer_id,
                "observer_instance_id": observer_instance_for(observer_id),
                "observer_domain": (
                    "performance_now_ns" if observer_id == "chromium" else "collector_monotonic_ns"
                ),
                "collector_domain": "collector_monotonic_ns",
                "offset_ns": 0,
                "calibration": {
                    "source": "collector_handshake",
                    "calibration_id": f"{observer_id}-calibration-1",
                    "before": {"observer_ns": start, "collector_ns": start},
                    "after": {"observer_ns": finish, "collector_ns": finish},
                },
            }
            for observer_id in observer_ids
        },
    }


def snapshot_clock(observer_id, captured):
    domain = "performance_now_ns" if observer_id == "chromium" else "collector_monotonic_ns"
    return {
        "mapping_id": f"{observer_id}-mapping",
        "observer_id": observer_id,
        "observer_instance_id": observer_instance_for(observer_id),
        "observer_domain": domain,
        "collector_domain": "collector_monotonic_ns",
        "observer_ns": captured,
        "collector_ns": captured,
        "offset_ns": 0,
    }


def checkpoint(checkpoint_id, after_action, expected_state, observer_id="gtk", time_constraint=None, action_index=None):
    return {
        "id": checkpoint_id,
        "after_action": after_action,
        "action_index": action_index,
        "expected_state": copy.deepcopy(expected_state),
        "observer_id": observer_id,
        "time_constraint": time_constraint or {"within_attempt": True},
    }


def materialize_expected(value, context):
    if isinstance(value, dict) and set(value) == {"$same_as"}:
        current = context
        for part in value["$same_as"].split("."):
            current = current[part]
        return copy.deepcopy(current)
    if isinstance(value, dict):
        return {key: materialize_expected(item, context) for key, item in value.items()}
    if isinstance(value, list):
        return [materialize_expected(item, context) for item in value]
    return copy.deepcopy(value)


def snapshot(observation_id, captured, state, case_id="manual", generation=None, observer_id="gtk", attempt_id="manual-1", run_id="run-1"):
    return {
        "observation_id": observation_id,
        "observer_id": observer_id,
        "observer_instance_id": observer_instance_for(observer_id),
        "state_generation": observation_id if generation is None else generation,
        "captured_monotonic_ns": captured,
        "run_id": run_id,
        "case_id": case_id,
        "attempt_id": attempt_id,
        "clock": snapshot_clock(observer_id, captured),
        "state": copy.deepcopy(state),
    }


def sign_attempt(value):
    for mapping in value["clock_mappings"].values():
        signature = TEST_COLLECTOR_PRIVATE_KEY.sign(_calibration_payload(mapping, value))
        mapping["calibration"]["signature_base64"] = base64.b64encode(signature).decode("ascii")
    for action in value.get("actions", []):
        signature = TEST_COLLECTOR_PRIVATE_KEY.sign(_action_payload(action, value))
        action["signature_base64"] = base64.b64encode(signature).decode("ascii")
    for observed in value.get("checkpoints", {}).values():
        confirmation = observed.get("completion_confirmation")
        if isinstance(confirmation, dict):
            signature = TEST_COLLECTOR_PRIVATE_KEY.sign(
                test_completion_confirmation_payload(confirmation)
            )
            confirmation["signature_base64"] = base64.b64encode(signature).decode("ascii")


def test_completion_confirmation_payload(confirmation):
    fields = (
        "task_id", "iteration", "plan_revision", "contract_id", "run_id",
        "case_id", "attempt_id", "artifact_id", "provenance_id",
        "environment_fingerprint",
    )
    value = {
        "version": 1,
        "confirmation_id": confirmation.get("confirmation_id"),
        "source": confirmation.get("source"),
        "correction_kind": confirmation.get("correction_kind"),
        "checkpoint_id": confirmation.get("checkpoint_id"),
        "observer_id": confirmation.get("observer_id"),
        "observer_instance_id": confirmation.get("observer_instance_id"),
        "observation_id": confirmation.get("observation_id"),
        "state_generation": confirmation.get("state_generation"),
        "trigger_action_index": confirmation.get("trigger_action_index"),
        "trigger_collector_event_id": confirmation.get("trigger_collector_event_id"),
        "result_state_sha256": confirmation.get("result_state_sha256"),
        "earliest_monotonic_ns": confirmation.get("earliest_monotonic_ns"),
        "latest_monotonic_ns": confirmation.get("latest_monotonic_ns"),
        "binding": {field: confirmation.get(field) for field in fields},
    }
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def add_completion_confirmation(value, checkpoint_id, trigger_action_index, earliest=None, latest=None):
    observed = value["checkpoints"][checkpoint_id]
    trigger = value["actions"][trigger_action_index]
    if earliest is None:
        trigger_completed = trigger["completed_monotonic_ns"]
        earliest = trigger_completed + (
            observed["captured_monotonic_ns"] - trigger_completed
        ) // 2
    latest = earliest if latest is None else latest
    fields = (
        "task_id", "iteration", "plan_revision", "contract_id", "run_id",
        "case_id", "attempt_id", "artifact_id", "provenance_id",
        "environment_fingerprint",
    )
    observed["completion_confirmation"] = {
        "confirmation_id": f"{value['attempt_id']}:{checkpoint_id}:completion",
        "source": "isolated_product_observer_transition",
        "correction_kind": "automatic_word_correction",
        "checkpoint_id": checkpoint_id,
        "observer_id": observed["observer_id"],
        "observer_instance_id": observed["observer_instance_id"],
        "observation_id": observed["observation_id"],
        "state_generation": observed["state_generation"],
        "trigger_action_index": trigger_action_index,
        "trigger_collector_event_id": trigger["collector_event_id"],
        "result_state_sha256": canonical_sha256(observed["state"]),
        "earliest_monotonic_ns": earliest,
        "latest_monotonic_ns": latest,
        **{field: value[field] for field in fields},
    }
    sign_attempt(value)
    return observed["completion_confirmation"]


def attempt(case=None, *, attempt_id="manual-1", start=100, finish=500, case_id=None, observer_id=None):
    case = case or base_case()
    case_id = case_id or case["id"]
    observer_id = observer_id or observer_id_for(case.get("surface"))
    specs = list(case.get("checkpoints") or [])
    after_state = case["expected_state"]
    next_state = case.get("next_input", {}).get("expected_state", after_state)
    before = snapshot(1, start + 1, case["initial_state"], case_id, observer_id=observer_id, attempt_id=attempt_id)
    context = {"before": before, "checkpoints": {}}
    after = snapshot(2, start + 15, after_state, case_id, observer_id=observer_id, attempt_id=attempt_id)
    after_next = snapshot(3, start + 35, next_state, case_id, observer_id=observer_id, attempt_id=attempt_id)
    evidence = {}
    if specs:
        for index, spec in enumerate(specs):
            action_index = spec["action_index"]
            captured = start + 15 + action_index * 20
            state = materialize_expected(spec["expected_state"], context)
            item = snapshot(
                index + 2,
                captured,
                state,
                case_id,
                observer_id=spec.get("observer_id", observer_id),
                attempt_id=attempt_id,
            )
            item["after_action"] = spec["after_action"]
            item["action_index"] = spec["action_index"]
            item["collector_event_id"] = f"{attempt_id}-action-{spec['action_index']}"
            evidence[spec["id"]] = item
            context["checkpoints"][spec["id"]] = item
            if index == 0:
                after = item
                context["after"] = item
            if spec is specs[-1] and case.get("next_input"):
                after_next = item
            elif spec.get("id") == "after_next_input":
                after_next = item
    else:
        evidence["after_trigger"] = after
        if case.get("next_input"):
            evidence["after_next_input"] = after_next
    value = {
        "attempt_id": attempt_id,
        "task_id": "c2c_bfdc",
        "iteration": 7,
        "plan_revision": contract()["plan_revision"],
        "contract_id": contract()["contract_id"],
        "run_id": "run-1",
        "environment_fingerprint": environment()["fingerprint_sha256"],
        "artifact_id": "candidate",
        "provenance_id": "candidate-provenance",
        "case_id": case_id,
        "attempt_index": 1,
        "input_origin": "physical",
        "input_origin_evidence": "isolated-seat-capture",
        "started_monotonic_ns": start,
        "finished_monotonic_ns": finish,
        "before": before,
        "after": after,
        "next_input": {"origin": "physical", "symbol": case.get("next_input", {}).get("symbol")},
        "after_next_input": after_next,
        "checkpoints": evidence,
        "actions": [
            {
                "action_index": index,
                "action": action,
                "started_monotonic_ns": start + 5 + index * 20,
                "completed_monotonic_ns": start + 10 + index * 20,
                "source": "isolated_input_collector",
                "collector_event_id": f"{attempt_id}-action-{index}",
            }
            for index, action in enumerate(
                [*case["steps"]]
                + ([f"physically_type:{case['next_input']['symbol']}"] if case.get("next_input") else [])
            )
        ],
        "clock_mappings": clock_mapping(
            start,
            finish,
            {observer_id, *(spec.get("observer_id", observer_id) for spec in specs)},
        )["observers"],
    }
    guard = case.get("rejection_guard")
    if isinstance(guard, dict):
        value["guard_evidence"] = {
            "type": guard["type"],
            "word": guard["word"],
            "dictionary_member": False,
            "dictionary_revision": value["provenance_id"],
            "checked_monotonic_ns": start + 15,
            **{
                field: value[field]
                for field in (
                    "task_id", "run_id", "case_id", "attempt_id", "artifact_id", "provenance_id",
                )
            },
        }
    for spec in specs:
        confirmation = spec.get("confirms_event")
        if isinstance(confirmation, dict):
            add_completion_confirmation(
                value,
                spec["id"],
                confirmation["trigger_action_index"],
            )
    sign_attempt(value)
    return value


def retime_attempt(value, case, start, observation_counters):
    cursor = start + 10_000_000
    action_times = {}
    for index, action in enumerate(value["actions"]):
        action_started = cursor + 5_000_000
        action["started_monotonic_ns"] = action_started
        if action["action"].startswith("wait_ms:"):
            cursor = action_started + int(action["action"].split(":", 1)[1]) * 1_000_000
        else:
            cursor = action_started + 5_000_000
        action["completed_monotonic_ns"] = cursor
        action_times[index] = cursor
    finish = cursor + 10_000_000
    value["started_monotonic_ns"] = start
    value["finished_monotonic_ns"] = finish
    snapshots = [value["before"], *value["checkpoints"].values()]
    value["before"]["captured_monotonic_ns"] = start + 1_000_000
    for spec in case["checkpoints"]:
        observed = value["checkpoints"][spec["id"]]
        observed["captured_monotonic_ns"] = action_times[spec["action_index"]] + 1_000_000
    observer_ids = {observed["observer_id"] for observed in snapshots}
    value["clock_mappings"] = clock_mapping(start, finish, observer_ids)["observers"]
    for observed in snapshots:
        observer_id = observed["observer_id"]
        observation_counters[observer_id] = observation_counters.get(observer_id, 0) + 1
        observed["observation_id"] = observation_counters[observer_id]
        observed["state_generation"] = observation_counters[observer_id]
        captured = observed["captured_monotonic_ns"]
        observed["clock"] = snapshot_clock(observer_id, captured)
    for spec in case["checkpoints"]:
        confirmation = spec.get("confirms_event")
        if isinstance(confirmation, dict):
            add_completion_confirmation(
                value,
                spec["id"],
                confirmation["trigger_action_index"],
            )
    sign_attempt(value)
    return finish


def collector_intervals(start=1):
    cursor = start
    active = []
    idle = []
    for index, duration in enumerate((400, 300, 400, 300, 400)):
        end = cursor + duration * NS
        item = {"start_ns": cursor, "end_ns": end, "duration_seconds": duration}
        (active if index % 2 == 0 else idle).append(item)
        cursor = end
    return start, cursor, active, idle


def soak_operation(index, identity, binding, case, monotonic_ns, observer_id="gtk"):
    attempt_id = f"soak-{index}"
    start = monotonic_ns
    finish = monotonic_ns + 100
    item = attempt(case, attempt_id=attempt_id, start=start, finish=finish, observer_id=observer_id)
    operation = {
        **item,
        "operation_id": attempt_id,
        "monotonic_ns": monotonic_ns,
        "case_id": case["id"],
        "result": "PASS",
        "process_identity": identity,
        **binding,
    }
    for snapshot_value in [operation["before"], *operation["checkpoints"].values()]:
        snapshot_value["observation_id"] += 10 + index * 10
    for spec in case.get("checkpoints", []):
        confirmation = spec.get("confirms_event")
        if isinstance(confirmation, dict):
            add_completion_confirmation(
                operation,
                spec["id"],
                confirmation["trigger_action_index"],
            )
    sign_attempt(operation)
    return operation


def r8_case(case_id="manual", *, surface="gtk_entry", repetitions=1):
    value = base_case(case_id)
    value["surface"] = surface
    value["repetitions"] = repetitions
    observer_id = observer_id_for(surface)
    value["checkpoints"] = [
        checkpoint("after-operation", value["steps"][-1], value["expected_state"], observer_id, action_index=len(value["steps"]) - 1),
        checkpoint(
            "after-next-input",
            f"physically_type:{value['next_input']['symbol']}",
            value["next_input"]["expected_state"],
            observer_id,
            action_index=len(value["steps"]),
        ),
    ]
    return value


def correction_completion_case(case_id="correction-completion", *, bound="max"):
    corrected = {
        "text": "привет ",
        "selection": [7, 7],
        "focus_target": "gtk_entry",
        "layout": {"gnome_source": "ru", "xkb_group": 1, "indicator": "ru"},
    }
    restored = {
        "text": "ghbdtn ",
        "selection": [7, 7],
        "focus_target": "gtk_entry",
        "layout": {"gnome_source": "en", "xkb_group": 0, "indicator": "en"},
    }
    relation = {
        "id": "correction-to-undo",
        "from_checkpoint_id": "after-automatic-correction",
        "from_event": "confirmed_correction_completed",
        "to_action_index": 2,
        "to_event": "started",
        ("max_ms" if bound == "max" else "min_ms"): (2500 if bound == "max" else 2501),
    }
    value = {
        "id": case_id,
        "capability": "P1_UNDO_LEARNING",
        "surface": "gtk_entry",
        "direction": "en_ru",
        "steps": ["physically_type:ghbdtn", "physically_press:Space", "physically_press:Ctrl+Z"],
        "initial_state": {
            "text": "",
            "selection": [0, 0],
            "focus_target": "gtk_entry",
            "layout": {"gnome_source": "en", "xkb_group": 0, "indicator": "en"},
        },
        "expected_state": restored if bound == "max" else corrected,
        "repetitions": 1,
        "requires_physical_input": True,
        "temporal_constraints": [relation],
        "checkpoints": [
            {
                "id": "after-automatic-correction",
                "after_action": "physically_press:Space",
                "action_index": 1,
                "observer_id": "gtk",
                "time_constraint": {"within_attempt": True},
                "expected_state": corrected,
                "confirms_event": {
                    "kind": "automatic_word_correction",
                    "trigger_action_index": 1,
                },
            },
            {
                "id": "after-undo",
                "after_action": "physically_press:Ctrl+Z",
                "action_index": 2,
                "observer_id": "gtk",
                "time_constraint": {"within_attempt": True},
                "expected_state": restored if bound == "max" else corrected,
            },
        ],
    }
    return value


def retime_correction_attempt(value, *, confirmation_earliest, confirmation_latest, undo_started):
    correction = value["checkpoints"]["after-automatic-correction"]
    correction_captured = confirmation_latest + 1_000_000
    correction["captured_monotonic_ns"] = correction_captured
    correction["clock"]["observer_ns"] = correction_captured
    correction["clock"]["collector_ns"] = correction_captured
    completed = undo_started + 1_000_000
    value["actions"][2]["started_monotonic_ns"] = undo_started
    value["actions"][2]["completed_monotonic_ns"] = completed
    after = value["checkpoints"]["after-undo"]
    after_captured = completed + 1_000_000
    after["captured_monotonic_ns"] = after_captured
    after["clock"]["observer_ns"] = after_captured
    after["clock"]["collector_ns"] = after_captured
    value["finished_monotonic_ns"] = completed + 10_000_000
    for mapping in value["clock_mappings"].values():
        mapping["calibration"]["after"] = {
            "observer_ns": value["finished_monotonic_ns"],
            "collector_ns": value["finished_monotonic_ns"],
        }
    add_completion_confirmation(
        value,
        "after-automatic-correction",
        1,
        earliest=confirmation_earliest,
        latest=confirmation_latest,
    )
    sign_attempt(value)


def complete_soak(case=None, **overrides):
    identity = {"pid": 41, "start_ticks": 99, "daemon_epoch": "epoch-1"}
    binding = {
        "run_id": "run-1",
        "artifact_id": "candidate",
        "provenance_id": "candidate-provenance",
        "environment_fingerprint": environment()["fingerprint_sha256"],
    }
    start, finish, active, idle = collector_intervals(10 * NS)
    cases = case if isinstance(case, list) else [case or base_case()]
    binding.update({
        "task_id": "c2c_bfdc",
        "iteration": 7,
        "plan_revision": contract()["plan_revision"],
        "contract_id": contract()["contract_id"],
    })
    mids = [interval["start_ns"] + 10 for interval in active]
    value = {
        "started_monotonic_ns": start,
        "finished_monotonic_ns": finish,
        "process_identity": identity,
        "process_restarts": 0,
        **binding,
        "active_intervals": active,
        "idle_intervals": idle,
        "operation_log": [
            soak_operation(
                index,
                identity,
                binding,
                cases[index % len(cases)],
                mids[index],
                observer_id_for(cases[index % len(cases)].get("surface")),
            )
            for index in range(3)
        ],
    }
    value.update(overrides)
    return value


def passing_soak_for_correction_case(case):
    soak = complete_soak(case)
    relation = case["temporal_constraints"][0]
    if "min_ms" in relation:
        for operation in soak["operation_log"]:
            correction_completed = operation["actions"][1]["completed_monotonic_ns"] + 100_000_000
            retime_correction_attempt(
                operation,
                confirmation_earliest=correction_completed,
                confirmation_latest=correction_completed,
                undo_started=correction_completed + relation["min_ms"] * 1_000_000,
            )
    return soak


def evaluate(value=None, attempts=None, env=None, actual_artifact=None, soak=None):
    active_contract = value or contract()
    default_soak_case = next(case for case in active_contract["cases"] if case.get("authorized", True))
    return evaluate_run(
        active_contract,
        env or environment(),
        actual_artifact or artifact(),
        attempts if attempts is not None else [attempt(default_soak_case)],
        soak if soak is not None else complete_soak(default_soak_case),
    )


class ProbeContractTests(unittest.TestCase):
    def test_complete_evidence_passes_without_granting_authority(self):
        report = evaluate()
        self.assertEqual("PASS", report["status"])
        self.assertEqual("PASS", report["soak_result"])
        self.assertEqual("none", report["execution_authority"])

    def test_delayed_correction_uses_signed_completion_not_space_time(self):
        case = correction_completion_case()
        value = contract()
        value["cases"] = [case]
        value["budgets"]["soak"]["operation_case_cycle"] = [case["id"]]
        item = attempt(case)
        space_completed = item["actions"][1]["completed_monotonic_ns"]
        correction_completed = space_completed + 300_000_000
        undo_started = correction_completed + 2_300_000_000
        retime_correction_attempt(
            item,
            confirmation_earliest=correction_completed,
            confirmation_latest=correction_completed,
            undo_started=undo_started,
        )

        report = evaluate(value, [item], soak=complete_soak(case))

        self.assertGreater(undo_started - space_completed, 2_500_000_000)
        self.assertEqual(2_300_000_000, undo_started - correction_completed)
        self.assertEqual("PASS", report["status"], report)

        late = attempt(case, attempt_id="correction-too-late")
        late_space_completed = late["actions"][1]["completed_monotonic_ns"]
        late_correction_completed = late_space_completed + 300_000_000
        retime_correction_attempt(
            late,
            confirmation_earliest=late_correction_completed,
            confirmation_latest=late_correction_completed,
            undo_started=late_correction_completed + 2_501_000_000,
        )
        late_report = evaluate(value, [late], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", late_report["status"], late_report)
        self.assertIn(
            "temporal_constraint_exceeded:correction-too-late:correction-to-undo",
            late_report["reasons"],
        )

    def test_correction_completion_max_and_min_boundaries(self):
        for bound, elapsed_ms, expected_status, expected_reason in (
            ("max", 2500, "PASS", None),
            ("max", 2501, "INVALID_OBSERVATION", "temporal_constraint_exceeded"),
            ("min", 2501, "PASS", None),
            ("min", 2500, "INVALID_OBSERVATION", "temporal_constraint_too_early"),
        ):
            with self.subTest(bound=bound, elapsed_ms=elapsed_ms):
                case = correction_completion_case(bound=bound)
                value = contract()
                value["cases"] = [case]
                value["budgets"]["soak"]["operation_case_cycle"] = [case["id"]]
                item = attempt(case)
                correction_completed = item["actions"][1]["completed_monotonic_ns"] + 100_000_000
                retime_correction_attempt(
                    item,
                    confirmation_earliest=correction_completed,
                    confirmation_latest=correction_completed,
                    undo_started=correction_completed + elapsed_ms * 1_000_000,
                )
                report = evaluate(value, [item], soak=passing_soak_for_correction_case(case))
                self.assertEqual(expected_status, report["status"], report)
                if expected_reason:
                    self.assertTrue(any(expected_reason in reason for reason in report["reasons"]))

    def test_correction_completion_interval_straddling_threshold_is_inconclusive(self):
        for bound, earliest_gap_ms, latest_gap_ms in (
            ("max", 2501, 2499),
            ("min", 2502, 2500),
        ):
            with self.subTest(bound=bound):
                case = correction_completion_case(bound=bound)
                value = contract()
                value["cases"] = [case]
                value["budgets"]["soak"]["operation_case_cycle"] = [case["id"]]
                item = attempt(case)
                earliest = item["actions"][1]["completed_monotonic_ns"] + 100_000_000
                latest = earliest + (earliest_gap_ms - latest_gap_ms) * 1_000_000
                undo_started = earliest + earliest_gap_ms * 1_000_000
                retime_correction_attempt(
                    item,
                    confirmation_earliest=earliest,
                    confirmation_latest=latest,
                    undo_started=undo_started,
                )
                report = evaluate(value, [item], soak=complete_soak(case))
                self.assertEqual("INVALID_OBSERVATION", report["status"], report)
                self.assertTrue(any("temporal_constraint_indeterminate" in reason for reason in report["reasons"]))
                self.assertEqual([], report["confirmed_product_failures"])

    def test_correction_completion_confirmation_fails_closed_when_missing_or_untrusted(self):
        case = correction_completion_case()
        value = contract()
        value["cases"] = [case]
        value["budgets"]["soak"]["operation_case_cycle"] = [case["id"]]
        for mutation, expected_reason in (
            ("missing", "missing_completion_confirmation"),
            ("narrowed_without_signature", "invalid_completion_confirmation_signature"),
            ("reversed", "invalid_completion_confirmation_interval"),
            ("before_trigger", "invalid_completion_confirmation_interval"),
            ("after_checkpoint", "invalid_completion_confirmation_interval"),
        ):
            with self.subTest(mutation=mutation):
                item = attempt(case)
                confirmation = item["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
                if mutation == "missing":
                    del item["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
                elif mutation == "narrowed_without_signature":
                    confirmation["earliest_monotonic_ns"] += 1
                elif mutation == "reversed":
                    confirmation["earliest_monotonic_ns"] = confirmation["latest_monotonic_ns"] + 1
                    sign_attempt(item)
                elif mutation == "before_trigger":
                    confirmation["earliest_monotonic_ns"] = item["actions"][1]["completed_monotonic_ns"] - 1
                    sign_attempt(item)
                else:
                    confirmation["latest_monotonic_ns"] = (
                        item["checkpoints"]["after-automatic-correction"]["captured_monotonic_ns"] + 1
                    )
                    sign_attempt(item)
                report = evaluate(value, [item], soak=complete_soak(case))
                self.assertEqual("INVALID_OBSERVATION", report["status"], report)
                self.assertTrue(any(expected_reason in reason for reason in report["reasons"]))

    def test_correction_completion_confirmation_binds_identity_trigger_and_result(self):
        case = correction_completion_case()
        value = contract()
        value["cases"] = [case]
        value["budgets"]["soak"]["operation_case_cycle"] = [case["id"]]
        mutations = [
            *((field, "completion_confirmation_binding_mismatch") for field in (
                "task_id", "iteration", "plan_revision", "contract_id", "run_id",
                "case_id", "attempt_id", "artifact_id", "provenance_id",
                "environment_fingerprint",
            )),
            ("source", "completion_confirmation_source_mismatch"),
            ("correction_kind", "completion_confirmation_kind_mismatch"),
            ("checkpoint_id", "completion_confirmation_checkpoint_mismatch"),
            ("trigger_action_index", "completion_confirmation_trigger_mismatch"),
            ("trigger_collector_event_id", "completion_confirmation_trigger_mismatch"),
            ("result_state_sha256", "completion_confirmation_result_mismatch"),
            ("observer_id", "completion_confirmation_observer_mismatch"),
            ("observer_instance_id", "completion_confirmation_observer_mismatch"),
            ("observation_id", "completion_confirmation_observer_mismatch"),
            ("state_generation", "completion_confirmation_observer_mismatch"),
        ]
        for field, expected_reason in mutations:
            with self.subTest(field=field):
                item = attempt(case)
                confirmation = item["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
                confirmation[field] = "foreign" if not isinstance(confirmation[field], int) else confirmation[field] + 1
                sign_attempt(item)
                report = evaluate(value, [item], soak=complete_soak(case))
                self.assertEqual("INVALID_OBSERVATION", report["status"], report)
                self.assertTrue(any(expected_reason in reason for reason in report["reasons"]))

    def test_correction_completion_relation_is_enforced_in_soak(self):
        case = correction_completion_case()
        value = contract()
        value["cases"] = [case]
        value["budgets"]["soak"]["operation_case_cycle"] = [case["id"]]
        soak = complete_soak(case)
        operation = soak["operation_log"][0]
        correction_completed = operation["actions"][1]["completed_monotonic_ns"] + 100_000_000
        retime_correction_attempt(
            operation,
            confirmation_earliest=correction_completed,
            confirmation_latest=correction_completed,
            undo_started=correction_completed + 2_501_000_000,
        )
        report = evaluate(value, [attempt(case)], soak=soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"], report)
        self.assertTrue(any(
            reason.startswith("soak:soak-0:temporal_constraint_exceeded")
            for reason in report["reasons"]
        ))

    def test_unproven_completion_anchor_cannot_publish_dependent_product_failure(self):
        case = correction_completion_case()
        value = contract()
        value["cases"] = [case]
        value["budgets"]["soak"]["operation_case_cycle"] = [case["id"]]
        for mutation in ("missing", "wrong_action", "foreign_binding", "late_capture"):
            with self.subTest(mutation=mutation):
                item = attempt(case)
                anchor = item["checkpoints"]["after-automatic-correction"]
                item["checkpoints"]["after-undo"]["state"]["text"] = "wrong"
                if mutation == "missing":
                    del item["checkpoints"]["after-automatic-correction"]
                elif mutation == "wrong_action":
                    anchor["after_action"] = "physically_press:Ctrl+Z"
                elif mutation == "foreign_binding":
                    anchor["run_id"] = "foreign-run"
                else:
                    captured = item["actions"][2]["started_monotonic_ns"] + 1
                    anchor["captured_monotonic_ns"] = captured
                    anchor["clock"]["observer_ns"] = captured
                    anchor["clock"]["collector_ns"] = captured
                report = evaluate(value, [item], soak=complete_soak(case))
                self.assertEqual("INVALID_OBSERVATION", report["status"], report)
                self.assertEqual([], report["confirmed_product_failures"], report)
                self.assertEqual("INVALID_OBSERVATION", report["case_results"][case["id"]])

    def test_unconfirmed_isolation_aborts_before_impact(self):
        env = environment(isolation={"confirmed": False, "affects_working_session": False})
        self.assertEqual("ABORTED_SAFETY", evaluate(env=env)["status"])

    def test_wrong_focus_or_selection_is_product_failure(self):
        for field, value in (("focus_target", "none"), ("selection", [0, 6])):
            item = attempt()
            item["after"]["state"][field] = value
            with self.subTest(field=field):
                self.assertEqual("FAIL_PRODUCT", evaluate(attempts=[item])["status"])

    def test_layout_source_xkb_and_indicator_are_each_checked(self):
        for field, value in (("gnome_source", "en"), ("xkb_group", 0), ("indicator", "en")):
            item = attempt()
            item["after"]["state"]["layout"][field] = value
            with self.subTest(field=field):
                self.assertEqual("FAIL_PRODUCT", evaluate(attempts=[item])["status"])

    def test_unchanged_layout_is_compared_with_before(self):
        value = contract()
        value["cases"][0]["expected_state"]["layout"] = {"$same_as": "before.state.layout"}
        value["cases"][0]["checkpoints"][0]["expected_state"]["layout"] = {"$same_as": "before.state.layout"}
        item = attempt(value["cases"][0])
        item["after"]["state"]["layout"] = {"gnome_source": "ru", "xkb_group": 1, "indicator": "ru"}
        self.assertEqual("FAIL_PRODUCT", evaluate(value, [item])["status"])

    def test_wrong_next_physical_symbol_or_checkpoint_is_product_failure(self):
        item = attempt()
        item["next_input"]["symbol"] = "a"
        self.assertEqual("FAIL_PRODUCT", evaluate(attempts=[item])["status"])
        item = attempt()
        item["after_next_input"]["state"]["text"] = "привет"
        self.assertEqual("FAIL_PRODUCT", evaluate(attempts=[item])["status"])

    def test_attempt_binding_and_timestamps_are_required(self):
        for field in ("artifact_id", "task_id", "contract_id", "environment_fingerprint"):
            item = attempt()
            item[field] = "wrong"
            with self.subTest(field=field):
                self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[item])["status"])
        item = attempt()
        del item["started_monotonic_ns"]
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[item])["status"])
        item = attempt()
        item["before"]["captured_monotonic_ns"] = "110"
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[item])["status"])

    def test_attempt_identity_and_observations_cannot_be_reused(self):
        duplicate = attempt(attempt_id="manual-1")
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[attempt(), duplicate])["status"])
        item = attempt()
        item["after_next_input"]["observation_id"] = item["after"]["observation_id"]
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[item])["status"])

    def test_invalid_repetitions_unknown_case_and_extra_attempt_fail_closed(self):
        value = contract()
        value["cases"][0]["repetitions"] = 0
        self.assertEqual("INVALID_OBSERVATION", evaluate(value, [])["status"])
        unknown = attempt(case_id="unknown", attempt_id="unknown-1")
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[attempt(), unknown])["status"])

    def test_environment_values_and_fingerprint_are_checked(self):
        details = {"session_type": "wayland", "seat": "seat0"}
        env = environment(details=details, fingerprint_sha256=canonical_sha256(details))
        self.assertEqual("BLOCKED_ENVIRONMENT", evaluate(env=env)["status"])
        env = environment(fingerprint_sha256="0" * 64)
        self.assertEqual("BLOCKED_ENVIRONMENT", evaluate(env=env)["status"])

    def test_artifact_provenance_is_checked_as_one_record(self):
        wrong = artifact(provenance={"package_sha256": "a" * 64, "payload_sha256": "c" * 64})
        self.assertEqual("BLOCKED_IDENTITY", evaluate(actual_artifact=wrong)["status"])

    def test_physical_origin_requires_evidence(self):
        env = environment(input_origin={"kind": "physical", "evidence": ""})
        self.assertEqual("BLOCKED_INPUT_ORIGIN", evaluate(env=env)["status"])

    def test_soak_requires_operation_log_and_continuous_process_identity(self):
        soak = complete_soak()
        del soak["operation_log"]
        self.assertEqual("INVALID_OBSERVATION", evaluate(soak=soak)["status"])
        soak = complete_soak()
        soak["operation_log"][1]["process_identity"] = {"pid": 42, "start_ticks": 100, "daemon_epoch": "epoch-2"}
        self.assertEqual("INVALID_OBSERVATION", evaluate(soak=soak)["status"])
        soak = complete_soak()
        soak["operation_log"][1]["artifact_id"] = "baseline"
        self.assertEqual("INVALID_OBSERVATION", evaluate(soak=soak)["status"])

    def test_diagnostic_replay_limit_and_reference_are_checked(self):
        failed = attempt(attempt_id="failure")
        failed["after"]["state"]["text"] = "wrong"
        binding = {key: failed[key] for key in ("task_id", "iteration", "plan_revision", "contract_id", "run_id", "artifact_id", "provenance_id", "environment_fingerprint")}
        replay1 = {"attempt_id": "diag-1", "diagnostic_of": "failure", "started_monotonic_ns": 600, "finished_monotonic_ns": 650, **binding}
        replay2 = {"attempt_id": "diag-2", "diagnostic_of": "failure", "started_monotonic_ns": 700, "finished_monotonic_ns": 750, **binding}
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[failed, replay1, replay2])["status"])
        missing = {"attempt_id": "diag", "diagnostic_of": "missing", "started_monotonic_ns": 600, "finished_monotonic_ns": 650, **binding}
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[attempt(), missing])["status"])

    def test_diagnostic_must_follow_failure_and_keep_binding(self):
        failed = attempt(attempt_id="failure")
        failed["after"]["state"]["text"] = "wrong"
        binding = {key: failed[key] for key in ("task_id", "iteration", "plan_revision", "contract_id", "run_id", "artifact_id", "provenance_id", "environment_fingerprint")}
        replay = {"attempt_id": "diag", "diagnostic_of": "failure", "started_monotonic_ns": 400, "finished_monotonic_ns": 450, **binding}
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[failed, replay])["status"])
        replay["started_monotonic_ns"] = 600
        replay["finished_monotonic_ns"] = 650
        self.assertEqual("FAIL_PRODUCT", evaluate(attempts=[failed, replay])["status"])

    def test_first_failure_is_chronological_not_contract_order(self):
        value = contract()
        second = base_case("second")
        value["cases"].append(second)
        later = attempt(value["cases"][0], attempt_id="later", start=300, finish=400)
        for offset, role in enumerate(("before", "after", "after_next_input"), start=4):
            later[role]["observation_id"] = offset
        later["after"]["state"]["text"] = "wrong-later"
        earlier = attempt(second, attempt_id="earlier", start=100, finish=200)
        earlier["after"]["state"]["text"] = "wrong-earlier"
        report = evaluate(value, [later, earlier])
        self.assertEqual("FAIL_PRODUCT", report["status"])
        self.assertEqual("earlier", report["first_failure_attempt_id"])

    def test_fresh_unchanged_state_is_valid_but_reused_observation_is_stale(self):
        value = contract()
        case = value["cases"][0]
        case["expected_state"] = copy.deepcopy(case["initial_state"])
        case.pop("next_input")
        case["checkpoints"] = [checkpoint(
            "after-operation", case["steps"][-1], case["expected_state"], "gtk",
            action_index=len(case["steps"]) - 1,
        )]
        item = attempt(case)
        item.pop("next_input")
        item.pop("after_next_input")
        item["after"]["state"] = copy.deepcopy(item["before"]["state"])
        item["after"]["state_generation"] = item["before"]["state_generation"]
        self.assertEqual("PASS", evaluate(value, [item])["status"])
        item["after"]["observation_id"] = item["before"]["observation_id"]
        self.assertEqual("INVALID_OBSERVATION", evaluate(value, [item])["status"])

    def test_snapshot_from_another_case_is_rejected(self):
        item = attempt()
        item["after"]["case_id"] = "other"
        self.assertEqual("INVALID_OBSERVATION", evaluate(attempts=[item])["status"])

    def test_unauthorized_lifecycle_case_is_reported_not_passed(self):
        value = contract()
        value["cases"].append({
            "id": "install", "surface": "host", "direction": "none",
            "steps": ["install"], "authorized": False,
            "expected_status": "NOT_RUN_AUTHORIZATION",
        })
        report = evaluate(value)
        self.assertEqual("PASS", report["status"])
        self.assertEqual("NOT_RUN_AUTHORIZATION", report["case_results"]["install"])


class RevisionEightResidualTests(unittest.TestCase):
    def test_collector_event_ids_are_unique_across_repetitions(self):
        case = r8_case(repetitions=2)
        value = contract()
        value["cases"] = [case]
        first = attempt(case, attempt_id="first")
        second = attempt(case, attempt_id="second")
        second["attempt_index"] = 2
        for observed in [second["before"], *second["checkpoints"].values()]:
            observed["observation_id"] += 10
        for first_action, second_action in zip(first["actions"], second["actions"]):
            second_action["collector_event_id"] = first_action["collector_event_id"]
        for spec in case["checkpoints"]:
            second["checkpoints"][spec["id"]]["collector_event_id"] = first["actions"][spec["action_index"]]["collector_event_id"]
        sign_attempt(second)
        report = evaluate(value, [first, second], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("invalid_collector_event" in reason for reason in report["reasons"]))

    def test_non_text_and_unknown_surface_oracles_are_typed(self):
        invalid_cases = [
            {
                "id": "cli-invalid", "surface": "cli", "direction": "none",
                "steps": ["run"], "initial_state": {"exit_code": None},
                "expected_state": {"exit_code": "zero"}, "repetitions": 1,
                "checkpoints": [checkpoint("done", "run", {"exit_code": "zero"}, "cli-observer", action_index=0)],
            },
            {
                "id": "profile-invalid", "surface": "isolated_profile", "direction": "none",
                "steps": ["reset"],
                "initial_state": {"text": 123, "learning_count": "one", "profile": []},
                "expected_state": {"text": 123, "learning_count": "one", "profile": []},
                "repetitions": 1,
                "checkpoints": [checkpoint(
                    "done", "reset", {"text": 123, "learning_count": "one", "profile": []},
                    "profile-observer", action_index=0,
                )],
            },
            {
                "id": "unknown", "surface": "unknown_surface", "direction": "none",
                "steps": ["run"], "initial_state": {"junk": 1}, "expected_state": {"junk": 1},
                "repetitions": 1,
                "checkpoints": [checkpoint("done", "run", {"junk": 1}, "unknown", action_index=0)],
            },
        ]
        for case in invalid_cases:
            value = contract()
            value["cases"] = [case]
            with self.subTest(surface=case["surface"]):
                self.assertTrue(_contract_errors(value))

    def test_oracle_schemas_reject_extra_fields_on_every_surface_family(self):
        root = Path(__file__).resolve().parents[2]
        repository = json.loads((root / "protocol/c2c-bfdc-i7-recovery-contract.json").read_text())
        for surface in ("gtk_entry", "isolated_profile", "tray", "cli"):
            case = copy.deepcopy(next(
                item for item in repository["cases"]
                if item.get("authorized", True) and item["surface"] == surface
            ))
            states = [case["initial_state"], case["expected_state"]]
            states.extend(spec["expected_state"] for spec in case["checkpoints"])
            if case.get("next_input"):
                states.append(case["next_input"]["expected_state"])
            for state in states:
                state["unexpected_claim"] = "attested-by-nobody"
            value = contract()
            value["cases"] = [case]
            with self.subTest(surface=surface):
                reasons = _contract_errors(value)
                self.assertTrue(any("unsupported_oracle_field" in reason for reason in reasons))

    def test_optional_text_surface_fields_are_typed(self):
        root = Path(__file__).resolve().parents[2]
        repository = json.loads((root / "protocol/c2c-bfdc-i7-recovery-contract.json").read_text())
        mutations = (
            ("gtk_entry", "following_text", 123),
            ("chromium_textarea", "sentinel_input", 123),
            ("scratch_terminal", "command_executed", "false"),
        )
        for surface, field, invalid in mutations:
            case = copy.deepcopy(next(
                item for item in repository["cases"]
                if item.get("authorized", True) and item["surface"] == surface
            ))
            case["expected_state"][field] = invalid
            case["checkpoints"][-1]["expected_state"][field] = invalid
            value = contract()
            value["cases"] = [case]
            with self.subTest(surface=surface, field=field):
                self.assertTrue(any(f"invalid_{field}" in reason for reason in _contract_errors(value)))

    def test_layout_oracle_rejects_nested_extra_fields(self):
        case = r8_case()
        states = [case["initial_state"], case["expected_state"], case["next_input"]["expected_state"]]
        states.extend(spec["expected_state"] for spec in case["checkpoints"])
        for state in states:
            state["layout"]["unexpected_claim"] = "attested-by-nobody"
        value = contract()
        value["cases"] = [case]
        reasons = _contract_errors(value)
        self.assertTrue(any("unsupported_layout_field" in reason for reason in reasons))

    def test_attempt_identity_is_unique_across_primary_and_soak(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        primary = attempt(case)
        soak = complete_soak(case)
        reused = soak["operation_log"][0]
        reused["attempt_id"] = primary["attempt_id"]
        for observed in [reused["before"], *reused["checkpoints"].values()]:
            observed["attempt_id"] = primary["attempt_id"]
        sign_attempt(reused)
        report = evaluate(value, [primary], soak=soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertIn("invalid_soak:attempt_ids", report["reasons"])

    def test_signed_mapping_cannot_confuse_observer_type_and_instance(self):
        case = r8_case(surface="chromium_input")
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        mapping = item["clock_mappings"]["chromium-instance-1"]
        mapping["observer_id"] = "gtk"
        mapping["observer_domain"] = "collector_monotonic_ns"
        for observed in [item["before"], *item["checkpoints"].values()]:
            observed["clock"]["observer_id"] = "gtk"
            observed["clock"]["observer_domain"] = "collector_monotonic_ns"
        sign_attempt(item)
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("observer_identity_mismatch" in reason for reason in report["reasons"]))

    def test_empty_action_sequence_and_unbound_checkpoint_are_invalid(self):
        case = r8_case()
        case["steps"] = []
        value = contract()
        value["cases"] = [case]
        self.assertEqual(
            "INVALID_OBSERVATION",
            evaluate(value, [attempt(case)], soak=complete_soak(case))["status"],
        )

    def test_missing_executed_action_cannot_be_replaced_by_checkpoint_label(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        del item["actions"][1]
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("invalid_action_log" in reason for reason in report["reasons"]))

    def test_surface_oracle_requires_focus_selection_and_complete_layout(self):
        case = r8_case()
        for state in (case["initial_state"], case["expected_state"], case["next_input"]["expected_state"]):
            state.clear()
            state["text"] = "only-text"
        for spec in case["checkpoints"]:
            spec["expected_state"] = {"text": "only-text"}
        value = contract()
        value["cases"] = [case]
        self.assertEqual(
            "INVALID_OBSERVATION",
            evaluate(value, [attempt(case)], soak=complete_soak(case))["status"],
        )

    def test_surface_oracle_rejects_present_but_meaningless_values(self):
        case = r8_case()
        invalid = {
            "text": "value",
            "selection": None,
            "focus_target": None,
            "layout": {"gnome_source": None, "xkb_group": None, "indicator": None},
        }
        case["initial_state"] = copy.deepcopy(invalid)
        case["expected_state"] = copy.deepcopy(invalid)
        case["next_input"]["expected_state"] = copy.deepcopy(invalid)
        for spec in case["checkpoints"]:
            spec["expected_state"] = copy.deepcopy(invalid)
        value = contract()
        value["cases"] = [case]
        self.assertEqual(
            "INVALID_OBSERVATION",
            evaluate(value, [attempt(case)], soak=complete_soak(case))["status"],
        )

    def test_next_input_oracle_must_equal_its_checkpoint(self):
        case = r8_case()
        case["next_input"]["expected_state"]["text"] = "divergent-owner"
        value = contract()
        value["cases"] = [case]
        self.assertEqual(
            "INVALID_OBSERVATION",
            evaluate(value, [attempt(case)], soak=complete_soak(case))["status"],
        )

    def test_each_active_soak_interval_must_contain_work(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        soak = complete_soak(case)
        third_start = soak["active_intervals"][2]["start_ns"]
        for index, operation in enumerate(soak["operation_log"]):
            new_start = third_start + 10 + index * 100
            delta = new_start - operation["started_monotonic_ns"]
            operation["started_monotonic_ns"] += delta
            operation["finished_monotonic_ns"] += delta
            operation["monotonic_ns"] += delta
            for observed in [operation["before"], *operation["checkpoints"].values()]:
                observed["captured_monotonic_ns"] += delta
                observed["clock"]["observer_ns"] += delta
                observed["clock"]["collector_ns"] += delta
        self.assertEqual(
            "INVALID_OBSERVATION",
            evaluate(value, [attempt(case)], soak=soak)["status"],
        )
        self.assertTrue(any("no_operation_in_active:0" in reason for reason in evaluate(value, [attempt(case)], soak=soak)["reasons"]))

    def test_soak_attempt_ids_are_unique(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        soak = complete_soak(case)
        duplicate = soak["operation_log"][0]["attempt_id"]
        soak["operation_log"][1]["attempt_id"] = duplicate
        for observed in [soak["operation_log"][1]["before"], *soak["operation_log"][1]["checkpoints"].values()]:
            observed["attempt_id"] = duplicate
        self.assertEqual(
            "INVALID_OBSERVATION",
            evaluate(value, [attempt(case)], soak=soak)["status"],
        )

    def test_dictionary_rejection_guard_is_not_a_valid_manual_pause_contract(self):
        case = r8_case()
        case["rejection_guard"] = {
            "type": "dictionary_membership_required",
            "word": "ghbdtn",
            "checkpoint_id": "after-operation",
            "dictionary_identity": "artifact_provenance_id",
        }
        value = contract()
        value["cases"] = [case]
        report = evaluate(value, [attempt(case)], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("invalid_contract:manual:rejection_guard" in reason for reason in report["reasons"]))

    def test_source_backed_no_active_word_guard_needs_no_dictionary_evidence(self):
        case = r8_case()
        case["product_guard"] = {
            "type": "no_active_word",
            "source": "EventLoop::queue_manual_word_edit",
            "checkpoint_id": "after-operation",
        }
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        self.assertNotIn("guard_evidence", item)
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("PASS", report["status"])

    def test_arbitrary_uncalibrated_clock_offset_is_invalid(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        mapping = item["clock_mappings"]["gtk-instance-1"]
        mapping["offset_ns"] = 123456
        for observed in [item["before"], *item["checkpoints"].values()]:
            observed["clock"]["offset_ns"] = 123456
            observed["clock"]["observer_ns"] -= 123456
        self.assertEqual(
            "INVALID_OBSERVATION",
            evaluate(value, [item], soak=complete_soak(case))["status"],
        )

    def test_self_consistent_but_unsigned_clock_rewrite_is_invalid(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        mapping = item["clock_mappings"]["gtk-instance-1"]
        offset = 123456
        mapping["offset_ns"] = offset
        for point in mapping["calibration"].values():
            if isinstance(point, dict) and "observer_ns" in point:
                point["observer_ns"] = point["collector_ns"] - offset
        for observed in [item["before"], *item["checkpoints"].values()]:
            observed["clock"]["offset_ns"] = offset
            observed["clock"]["observer_ns"] = observed["clock"]["collector_ns"] - offset
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("signature_unconfirmed" in reason for reason in report["reasons"]))

    def test_missing_required_intermediate_checkpoint_cannot_pass(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        del item["checkpoints"]["after-operation"]
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertNotEqual("PASS", report["case_results"].get(case["id"]))

    def test_confirmed_wrong_text_survives_missing_later_snapshot(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        item["after"]["state"]["text"] = "wrong"
        item["checkpoints"]["after-operation"]["state"]["text"] = "wrong"
        del item["after_next_input"]
        del item["checkpoints"]["after-next-input"]
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertIn(item["attempt_id"], report["confirmed_product_failures"])
        self.assertEqual("FAIL_PRODUCT", report["case_results"].get(case["id"]))

    def test_incomplete_repetitions_never_publish_case_pass(self):
        case = r8_case(repetitions=2)
        value = contract()
        value["cases"] = [case]
        report = evaluate(value, [attempt(case)], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertEqual("INVALID_OBSERVATION", report["case_results"].get(case["id"]))

    def test_foreign_binding_never_publishes_case_pass(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        item["artifact_id"] = "baseline"
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertEqual("INVALID_OBSERVATION", report["case_results"].get(case["id"]))

    def test_empty_required_oracle_is_invalid_contract(self):
        case = r8_case()
        case["initial_state"] = {}
        case["expected_state"] = {}
        for spec in case["checkpoints"]:
            spec["expected_state"] = {}
        value = contract()
        value["cases"] = [case]
        report = evaluate(value, [attempt(case)], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("empty_oracle" in reason for reason in report["reasons"]))

    def test_soak_operation_without_result_is_invalid(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        soak = complete_soak(case)
        del soak["operation_log"][1]["result"]
        self.assertEqual("INVALID_OBSERVATION", evaluate(value, [attempt(case)], soak=soak)["status"])

    def test_soak_product_failure_after_idle_is_preserved(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        soak = complete_soak(case)
        failed = soak["operation_log"][1]
        failed["after"]["state"]["text"] = "wrong"
        failed["checkpoints"]["after-operation"]["state"]["text"] = "wrong"
        failed["result"] = "FAIL_PRODUCT"
        report = evaluate(value, [attempt(case)], soak=soak)
        self.assertEqual("FAIL_PRODUCT", report["status"])
        self.assertIn(failed["attempt_id"], report["confirmed_product_failures"])

    def test_soak_safety_event_aborts(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        soak = complete_soak(case)
        soak["operation_log"][1]["safety_events"] = ["lost_text"]
        self.assertEqual("ABORTED_SAFETY", evaluate(value, [attempt(case)], soak=soak)["status"])

    def test_equal_local_ids_from_different_observers_are_valid(self):
        gtk_case = r8_case("gtk", surface="gtk_entry")
        chromium_case = r8_case("chromium", surface="chromium_input")
        value = contract()
        value["cases"] = [gtk_case, chromium_case]
        value["budgets"]["soak"]["operation_case_cycle"] = [gtk_case["id"]]
        gtk = attempt(gtk_case, attempt_id="gtk-1")
        chromium = attempt(chromium_case, attempt_id="chromium-1")
        report = evaluate(value, [gtk, chromium], soak=complete_soak(gtk_case))
        self.assertEqual("PASS", report["status"])

    def test_equal_local_ids_from_two_instances_of_same_observer_are_valid(self):
        case = r8_case(repetitions=2)
        value = contract()
        value["cases"] = [case]
        first = attempt(case, attempt_id="first")
        second = attempt(case, attempt_id="second")
        second["attempt_index"] = 2
        old_instance = "gtk-instance-1"
        new_instance = "gtk-instance-2"
        mapping = second["clock_mappings"].pop(old_instance)
        mapping["observer_instance_id"] = new_instance
        mapping["mapping_id"] = "gtk-mapping-2"
        second["clock_mappings"][new_instance] = mapping
        for observed in [second["before"], *second["checkpoints"].values()]:
            observed["observer_instance_id"] = new_instance
            observed["clock"]["observer_instance_id"] = new_instance
            observed["clock"]["mapping_id"] = "gtk-mapping-2"
        sign_attempt(second)
        report = evaluate(value, [first, second], soak=complete_soak(case))
        self.assertEqual("PASS", report["status"])

    def test_missing_clock_mapping_is_invalid(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        del item["checkpoints"]["after-operation"]["clock"]
        self.assertEqual("INVALID_OBSERVATION", evaluate(value, [item], soak=complete_soak(case))["status"])

    def test_mixed_runs_are_invalid(self):
        case = r8_case(repetitions=2)
        value = contract()
        value["cases"] = [case]
        first = attempt(case, attempt_id="first")
        second = attempt(case, attempt_id="second")
        second["attempt_index"] = 2
        second["run_id"] = "run-2"
        for role in ("before", "after", "after_next_input"):
            second[role]["run_id"] = "run-2"
        for item in second["checkpoints"].values():
            item["run_id"] = "run-2"
        self.assertEqual(
            "INVALID_OBSERVATION",
            evaluate(value, [first, second], soak=complete_soak(case))["status"],
        )

    def test_action_to_action_deadline_is_not_checkpoint_latency(self):
        case = r8_case()
        case["temporal_constraints"] = [{
            "id": "rapid-tail-gap",
            "from_action_index": 1,
            "from_event": "completed",
            "to_action_index": 2,
            "to_event": "started",
            "max_ms": 100,
        }]
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        item["actions"][0]["started_monotonic_ns"] = 500_100
        item["actions"][0]["completed_monotonic_ns"] = 1_000_100
        item["actions"][1]["started_monotonic_ns"] = 99_000_100
        item["actions"][1]["completed_monotonic_ns"] = 100_000_100
        item["actions"][2]["started_monotonic_ns"] = 201_000_100
        item["actions"][2]["completed_monotonic_ns"] = 202_000_100
        item["finished_monotonic_ns"] = 203_000_100
        for checkpoint_id, captured in (
            ("after-operation", 100_000_101),
            ("after-next-input", 202_000_101),
        ):
            observed = item["checkpoints"][checkpoint_id]
            observed["captured_monotonic_ns"] = captured
            observed["clock"]["observer_ns"] = captured
            observed["clock"]["collector_ns"] = captured
        for mapping in item["clock_mappings"].values():
            mapping["calibration"]["after"] = {
                "observer_ns": item["finished_monotonic_ns"],
                "collector_ns": item["finished_monotonic_ns"],
            }
        sign_attempt(item)
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("temporal_constraint_exceeded" in reason for reason in report["reasons"]))

    def test_action_relation_boundaries_are_inclusive(self):
        for bound, gap_ms, expected_status, expected_reason in (
            ("max_ms", 100, "PASS", None),
            ("min_ms", 2501, "PASS", None),
            ("min_ms", 2500, "INVALID_OBSERVATION", "temporal_constraint_too_early"),
        ):
            with self.subTest(bound=bound, gap_ms=gap_ms):
                case = r8_case()
                case["temporal_constraints"] = [{
                    "id": "boundary",
                    "from_action_index": 1,
                    "from_event": "completed",
                    "to_action_index": 2,
                    "to_event": "started",
                    bound: 100 if bound == "max_ms" else 2501,
                }]
                value = contract()
                value["cases"] = [case]
                item = attempt(case)
                lower = 100_000_000
                upper_start = lower + gap_ms * 1_000_000
                item["actions"][0]["started_monotonic_ns"] = 49_000_000
                item["actions"][0]["completed_monotonic_ns"] = 50_000_000
                item["actions"][1]["started_monotonic_ns"] = lower - 1_000_000
                item["actions"][1]["completed_monotonic_ns"] = lower
                item["actions"][2]["started_monotonic_ns"] = upper_start
                item["actions"][2]["completed_monotonic_ns"] = upper_start + 1_000_000
                for checkpoint_id, captured in (
                    ("after-operation", lower + 1),
                    ("after-next-input", upper_start + 1_000_001),
                ):
                    observed = item["checkpoints"][checkpoint_id]
                    observed["captured_monotonic_ns"] = captured
                    observed["clock"]["observer_ns"] = captured
                    observed["clock"]["collector_ns"] = captured
                item["finished_monotonic_ns"] = upper_start + 1_000_010
                for mapping in item["clock_mappings"].values():
                    mapping["calibration"]["after"] = {
                        "observer_ns": item["finished_monotonic_ns"],
                        "collector_ns": item["finished_monotonic_ns"],
                    }
                sign_attempt(item)
                soak = complete_soak(case)
                if bound == "min_ms":
                    for operation in soak["operation_log"]:
                        started = operation["actions"][1]["completed_monotonic_ns"] + 2_501_000_000
                        completed = started + 1_000_000
                        operation["actions"][2]["started_monotonic_ns"] = started
                        operation["actions"][2]["completed_monotonic_ns"] = completed
                        observed = operation["checkpoints"]["after-next-input"]
                        observed["captured_monotonic_ns"] = completed + 1
                        observed["clock"]["observer_ns"] = completed + 1
                        observed["clock"]["collector_ns"] = completed + 1
                        operation["finished_monotonic_ns"] = completed + 10
                        for mapping in operation["clock_mappings"].values():
                            mapping["calibration"]["after"] = {
                                "observer_ns": operation["finished_monotonic_ns"],
                                "collector_ns": operation["finished_monotonic_ns"],
                            }
                        sign_attempt(operation)
                report = evaluate(value, [item], soak=soak)
                self.assertEqual(expected_status, report["status"])
                if expected_reason:
                    self.assertTrue(any(expected_reason in reason for reason in report["reasons"]))

    def test_checkpoint_cannot_be_captured_after_next_action(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        observed = item["checkpoints"]["after-operation"]
        captured = item["actions"][2]["started_monotonic_ns"] + 1
        observed["captured_monotonic_ns"] = captured
        observed["clock"]["observer_ns"] = captured
        observed["clock"]["collector_ns"] = captured
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("checkpoint_after_later_action" in reason for reason in report["reasons"]))

    def test_soak_operation_uses_common_five_second_budget(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        soak = complete_soak(case)
        operation = soak["operation_log"][0]
        operation["finished_monotonic_ns"] = operation["started_monotonic_ns"] + 6 * NS
        sign_attempt(operation)
        report = evaluate(value, [attempt(case)], soak=soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("oracle_budget_exceeded" in reason for reason in report["reasons"]))

    def test_exact_five_second_budget_passes_primary_and_soak(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        primary = attempt(case)
        primary["finished_monotonic_ns"] = primary["started_monotonic_ns"] + 5 * NS
        for mapping in primary["clock_mappings"].values():
            mapping["calibration"]["after"] = {
                "observer_ns": primary["finished_monotonic_ns"],
                "collector_ns": primary["finished_monotonic_ns"],
            }
        sign_attempt(primary)
        soak = complete_soak(case)
        operation = soak["operation_log"][0]
        operation["finished_monotonic_ns"] = operation["started_monotonic_ns"] + 5 * NS
        for mapping in operation["clock_mappings"].values():
            mapping["calibration"]["after"] = {
                "observer_ns": operation["finished_monotonic_ns"],
                "collector_ns": operation["finished_monotonic_ns"],
            }
        sign_attempt(operation)
        self.assertEqual("PASS", evaluate(value, [primary], soak=soak)["status"])

    def test_wrong_trusted_prefix_survives_missing_action_tail(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        item["checkpoints"]["after-operation"]["state"]["text"] = "wrong"
        item["actions"].pop()
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertEqual("FAIL_PRODUCT", report["case_results"][case["id"]])
        self.assertIn(item["attempt_id"], report["confirmed_product_failures"])

    def test_untrusted_prefix_never_publishes_product_failure(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        item = attempt(case)
        item["checkpoints"]["after-operation"]["state"]["text"] = "wrong"
        item["actions"].pop()
        item["actions"][1]["signature_base64"] = "invalid"
        report = evaluate(value, [item], soak=complete_soak(case))
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertNotIn(item["attempt_id"], report["confirmed_product_failures"])

    def test_wrong_soak_prefix_survives_truncated_log(self):
        case = r8_case()
        value = contract()
        value["cases"] = [case]
        soak = complete_soak(case)
        failed = soak["operation_log"][0]
        failed["checkpoints"]["after-operation"]["state"]["text"] = "wrong"
        failed["result"] = "FAIL_PRODUCT"
        soak["operation_log"].pop()
        report = evaluate(value, [attempt(case)], soak=soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertIn(failed["attempt_id"], report["confirmed_product_failures"])


class ObservationProtocolTests(unittest.TestCase):
    @staticmethod
    def mapping(protocol):
        return {
            "mapping_id": "test-calibration",
            "observer_id": protocol.observer_id,
            "observer_instance_id": protocol.observer_instance_id,
            "observer_domain": "collector_monotonic_ns",
            "collector_domain": "collector_monotonic_ns",
            "offset_ns": 0,
            "calibration": {
                "source": "collector_handshake",
                "calibration_id": "protocol-test-calibration",
                "before": {"observer_ns": 0, "collector_ns": 0},
                "after": {"observer_ns": 2**63 - 1, "collector_ns": 2**63 - 1},
            },
        }

    def test_fresh_reads_increment_observation_not_state_generation(self):
        protocol = ObservationProtocol()
        protocol.arm("run", "case", "attempt", self.mapping(protocol))
        first = protocol.snapshot({"text": "same"})
        second = protocol.snapshot({"text": "same"})
        self.assertGreater(second["observation_id"], first["observation_id"])
        self.assertEqual(first["state_generation"], second["state_generation"])
        self.assertEqual("run", second["run_id"])
        self.assertEqual("case", second["case_id"])
        self.assertEqual("attempt", second["attempt_id"])
        self.assertEqual("gtk", second["observer_id"])

    def test_prepare_during_active_case_is_forbidden(self):
        protocol = ObservationProtocol()
        protocol.arm("run", "case", "attempt", self.mapping(protocol))
        with self.assertRaises(RuntimeError):
            protocol.ensure_prepare_allowed()
        protocol.complete()
        protocol.ensure_prepare_allowed()


class ChromiumObserverProtocolTests(unittest.TestCase):
    def test_fresh_unchanged_reads_and_active_prepare_guard_with_dom_double(self):
        root = Path(__file__).resolve().parents[2]
        html = (root / "tests/recovery/chromium_target.html").read_text(encoding="utf-8")
        script = html.split("<script>", 1)[1].split("</script>", 1)[0]
        harness = r'''
class FakeElement {
  constructor() { this.value = ""; this.selectionStart = 0; this.selectionEnd = 0; this.listeners = {}; }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  setSelectionRange(start, end) { this.selectionStart = start; this.selectionEnd = end; }
  focus() { document.activeElement = this; if (this.listeners.focus) this.listeners.focus(); }
}
const elements = {"#scratch-input": new FakeElement(), "#scratch-textarea": new FakeElement(), "#tab-sentinel": new FakeElement()};
global.document = {
  activeElement: null,
  querySelector: selector => elements[selector],
  listeners: {},
  addEventListener(name, callback) { (this.listeners[name] ??= []).push(callback); },
  dispatchEvent(event) { for (const callback of this.listeners[event.type] || []) callback(event); },
};
global.window = {};
let clock = 1;
global.performance = {now: () => clock++};
''' + script + r'''
window.puntoProbe.prepare("input", "same");
const observerIdentity = window.puntoProbe.observerIdentity();
window.puntoProbe.arm("run", "case", "attempt", {
  mapping_id: "chromium-mapping",
  observer_id: "chromium",
  observer_instance_id: observerIdentity.observer_instance_id,
  observer_domain: "performance_now_ns",
  collector_domain: "collector_monotonic_ns",
  offset_ns: 0,
  calibration: {
    source: "collector_handshake",
    calibration_id: "chromium-test-calibration",
    before: {observer_ns: 0, collector_ns: 0},
    after: {observer_ns: 10000000, collector_ns: 10000000},
  },
});
const first = window.puntoProbe.snapshot("input");
const second = window.puntoProbe.snapshot("input");
if (JSON.stringify(first.state.keys) !== JSON.stringify({tab_down: 0, tab_up: 0, z_down: 0, z_up: 0})) throw new Error("initial key counts were not zero");
for (const code of ["Tab", "KeyZ", "KeyA"]) document.dispatchEvent({type: "keydown", code});
const pressed = window.puntoProbe.snapshot("input");
if (JSON.stringify(pressed.state.keys) !== JSON.stringify({tab_down: 1, tab_up: 0, z_down: 1, z_up: 0})) throw new Error("keydown counts were not exact");
for (const code of ["Tab", "KeyZ", "KeyA"]) document.dispatchEvent({type: "keyup", code});
const released = window.puntoProbe.snapshot("input");
const unchanged = window.puntoProbe.snapshot("input");
if (JSON.stringify(released.state.keys) !== JSON.stringify({tab_down: 1, tab_up: 1, z_down: 1, z_up: 1})) throw new Error("keyup counts were not exact");
if (JSON.stringify(first.state.keys) !== JSON.stringify(second.state.keys)) throw new Error("read changed initial key counts");
if (JSON.stringify(released.state.keys) !== JSON.stringify(unchanged.state.keys)) throw new Error("read changed released key counts");
if (JSON.stringify(pressed.state.keys) !== JSON.stringify({tab_down: 1, tab_up: 0, z_down: 1, z_up: 0})) throw new Error("later events changed an earlier snapshot");
const reads = [first, second, pressed, released, unchanged];
for (let index = 1; index < reads.length; index++) {
  const previous = reads[index - 1], current = reads[index];
  if (!(current.observation_id > previous.observation_id)) throw new Error("observation freshness did not advance");
  if (!(current.captured_monotonic_ns > previous.captured_monotonic_ns)) throw new Error("capture timestamp did not advance");
  if (!(current.clock.observer_ns > previous.clock.observer_ns)) throw new Error("observer timestamp did not advance");
  if (current.state_generation !== previous.state_generation) throw new Error("read changed state generation");
}
if (second.run_id !== "run" || second.case_id !== "case") throw new Error("binding missing");
if (second.attempt_id !== "attempt" || second.observer_id !== "chromium") throw new Error("attempt binding missing");
if (second.observer_instance_id !== observerIdentity.observer_instance_id) throw new Error("instance binding missing");
if (second.clock.collector_ns !== second.captured_monotonic_ns) throw new Error("clock mapping missing");
let blocked = false;
try { window.puntoProbe.prepare("input", "replacement"); } catch (_) { blocked = true; }
if (!blocked) throw new Error("active preparation was allowed");
window.puntoProbe.complete();
'''
        completed = subprocess.run(["node", "-e", harness], check=False, capture_output=True, text=True)
        self.assertEqual(0, completed.returncode, completed.stderr)


class RepositoryContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.value = json.loads((root / "protocol/c2c-bfdc-i7-recovery-contract.json").read_text(encoding="utf-8"))

    def test_contract_is_semantically_valid_and_has_no_placeholders(self):
        self.assertEqual([], _contract_errors(self.value))
        self.assertEqual(10, self.value["plan_revision"])
        self.assertEqual("punto-c2c-bfdc-i7-r10", self.value["contract_id"])
        authorized = [case for case in self.value["cases"] if case.get("authorized", True)]
        self.assertTrue(all(case.get("checkpoints") for case in authorized))

    def test_main_p0_matrix_is_complete(self):
        main = {
            (item["surface"], item["direction"], item["steps"][-1].split(":")[-1])
            for item in self.value["cases"]
            if item.get("capability") in {"P0_MANUAL_WORD", "P0_AUTO_WORD"}
        }
        expected = set(product(
            {"gtk_entry", "gtk_text_view", "chromium_input", "chromium_textarea"},
            {"en_ru", "ru_en"},
            {"Pause", "Space", "Tab"},
        ))
        self.assertEqual(expected, main)

    def test_preparation_actions_start_from_an_empty_text_target(self):
        text_surfaces = {
            "gtk_entry", "gtk_text_view", "chromium_input", "chromium_textarea", "scratch_terminal",
        }
        prepared = [
            case for case in self.value["cases"]
            if case.get("authorized", True)
            and case.get("surface") in text_surfaces
            and case.get("steps")
            and (
                case["steps"][0].startswith("physically_type:")
                or case["steps"][0].startswith("prepare_text:")
            )
        ]
        self.assertTrue(prepared)
        self.assertTrue(all(case["initial_state"]["text"] == "" for case in prepared))
        self.assertTrue(all(case["initial_state"]["selection"] == [0, 0] for case in prepared))

    def test_modifier_routes_and_tab_focus_are_explicit(self):
        ids = {case["id"] for case in self.value["cases"]}
        self.assertTrue({
            "shift-pause-selection-layout", "ctrl-pause-word-case",
            "alt-pause-selection-case", "ctrl-alt-pause-transliteration",
        }.issubset(ids))
        tab_cases = [case for case in self.value["cases"] if case["id"].endswith("-tab")]
        self.assertEqual(8, len(tab_cases))
        self.assertTrue(all(case["expected_state"].get("focus_target") for case in tab_cases))

    def test_lifecycle_cases_are_explicitly_unauthorized(self):
        lifecycle = [case for case in self.value["cases"] if case.get("capability") == "LIFECYCLE"]
        self.assertGreaterEqual(len(lifecycle), 3)
        self.assertTrue(all(case.get("authorized") is False for case in lifecycle))
        self.assertTrue(all(case.get("expected_status") == "NOT_RUN_AUTHORIZATION" for case in lifecycle))

    def test_multistep_recovery_undo_learning_and_browser_coverage_are_explicit(self):
        by_id = {case["id"]: case for case in self.value["cases"]}
        for suffix, surface, observer in (("", "gtk_entry", "gtk"), ("-chromium", "chromium_input", "chromium")):
            manual = by_id[f"manual-unknown-word-inversion{suffix}"]
            self.assertEqual(surface, manual["surface"])
            self.assertEqual("", manual["initial_state"]["text"])
            self.assertEqual("physically_type:qzx", manual["steps"][0])
            self.assertEqual("йяч", manual["expected_state"]["text"])
            self.assertEqual("йячф", manual["next_input"]["expected_state"]["text"])
            self.assertNotIn("rejection_guard", manual)
            rejection = by_id[f"safe-no-active-word-then-recovery{suffix}"]
            self.assertEqual("no_active_word", rejection["product_guard"]["type"])
            self.assertEqual("EventLoop::queue_manual_word_edit", rejection["product_guard"]["source"])
            self.assertEqual(observer, rejection["checkpoints"][0]["observer_id"])
            self.assertEqual("", rejection["checkpoints"][0]["expected_state"]["text"])
            self.assertEqual("привет", rejection["expected_state"]["text"])
            self.assertEqual("приветф", rejection["next_input"]["expected_state"]["text"])
        self.assertNotIn("dictionary_membership_required", json.dumps(self.value, ensure_ascii=False))
        self.assertEqual(
            [0, 1, 0],
            [
                by_id["isolated-learning-reset"]["initial_state"]["learning_count"],
                by_id["isolated-learning-reset"]["checkpoints"][1]["expected_state"]["learning_count"],
                by_id["isolated-learning-reset"]["expected_state"]["learning_count"],
            ],
        )
        learning = by_id["isolated-learning-reset"]
        self.assertEqual(
            [
                "physically_type:ghbdtn", "physically_press:Space",
                "physically_press:Ctrl+Z", "verify_learning_count:1", "reset_learning",
            ],
            learning["steps"],
        )
        self.assertEqual("привет ", learning["checkpoints"][0]["expected_state"]["text"])
        self.assertEqual("ghbdtn ", learning["checkpoints"][1]["expected_state"]["text"])
        undo_relation = by_id["punto-undo-within-2500ms"]["temporal_constraints"][0]
        self.assertEqual(("after-automatic-correction", 2, 2500), (
            undo_relation["from_checkpoint_id"], undo_relation["to_action_index"], undo_relation["max_ms"],
        ))
        expiry_relation = by_id["punto-undo-expired-without-input"]["temporal_constraints"][0]
        self.assertEqual(("after-automatic-correction", 3, 2501), (
            expiry_relation["from_checkpoint_id"], expiry_relation["to_action_index"], expiry_relation["min_ms"],
        ))
        for identifier in (
            "punto-undo-within-2500ms",
            "punto-undo-expired-without-input",
            "isolated-learning-reset",
        ):
            confirmation = by_id[identifier]["checkpoints"][0]["confirms_event"]
            self.assertEqual("automatic_word_correction", confirmation["kind"])
            self.assertEqual(1, confirmation["trigger_action_index"])
        invalidated = by_id["punto-undo-invalidated-by-ordinary-input"]
        self.assertEqual("привет ф", invalidated["expected_state"]["text"])
        self.assertEqual([8, 8], invalidated["expected_state"]["selection"])
        for identifier in ("rapid-tail-preserved", "rapid-tail-preserved-chromium"):
            rapid = by_id[identifier]
            self.assertEqual("привет ", rapid["checkpoints"][0]["expected_state"]["text"])
            self.assertEqual("привет ф", rapid["expected_state"]["text"])
            self.assertEqual(100, rapid["temporal_constraints"][0]["max_ms"])
        for identifier in (
            "negative-correct-word", "rapid-tail-preserved", "manual-unknown-word-inversion",
            "safe-no-active-word-then-recovery",
            "shift-pause-selection-layout", "ctrl-pause-word-case",
            "alt-pause-selection-case", "ctrl-alt-pause-transliteration",
        ):
            self.assertIn(f"{identifier}-chromium", by_id)

    def _synthetic_fixture(self):
        value = copy.deepcopy(self.value)
        value["collector_attestation"] = {
            "algorithm": "ed25519",
            "public_key_base64": TEST_COLLECTOR_PUBLIC_KEY,
            "key_id": "synthetic-test-collector",
        }
        selected_ids = ["rapid-tail-preserved", "chromium-input-en-ru-pause"]
        selected = [copy.deepcopy(next(case for case in value["cases"] if case["id"] == identifier)) for identifier in selected_ids]
        for case in selected:
            case["repetitions"] = 1
        value["cases"] = selected
        value["budgets"]["soak"] = {
            "elapsed_seconds": 1800,
            "active_seconds": 1200,
            "idle_intervals_seconds": [300, 300],
            "operations": 3,
            "process_restarts": 0,
            "operation_case_cycle": selected_ids,
        }
        details = copy.deepcopy(value["supported_environment"]["expected"])
        for field in value["supported_environment"]["required_present"]:
            details[field] = f"synthetic-{field}"
        env = {
            "isolation": {"confirmed": True, "affects_working_session": False},
            "details": details,
            "fingerprint_sha256": canonical_sha256(details),
            "input_origin": {"kind": "physical", "evidence": "synthetic-physical-capture"},
        }
        artifact_id = "candidate_2_8_14"
        expected_artifact = value["artifacts"][artifact_id]
        actual_artifact = {"id": artifact_id, **copy.deepcopy(expected_artifact)}
        attempts = [
            attempt(selected[0], attempt_id="synthetic-gtk", start=100, finish=500),
            attempt(selected[1], attempt_id="synthetic-chromium", start=600, finish=1000),
        ]
        soak = complete_soak(selected)
        binding = {
            "task_id": value["task_id"],
            "iteration": value["iteration"],
            "plan_revision": value["plan_revision"],
            "contract_id": value["contract_id"],
            "run_id": "run-1",
            "artifact_id": artifact_id,
            "provenance_id": actual_artifact["provenance_id"],
            "environment_fingerprint": env["fingerprint_sha256"],
        }
        for item in attempts:
            item.update(binding)
            item["input_origin_evidence"] = env["input_origin"]["evidence"]
            sign_attempt(item)
        soak.update(binding)
        for item in soak["operation_log"]:
            item.update(binding)
            item["input_origin_evidence"] = env["input_origin"]["evidence"]
            sign_attempt(item)
        return value, env, actual_artifact, attempts, soak

    @staticmethod
    def _full_environment(value):
        details = copy.deepcopy(value["supported_environment"]["expected"])
        for field in value["supported_environment"]["required_present"]:
            details[field] = f"offline-{field}"
        return {
            "isolation": {"confirmed": True, "affects_working_session": False},
            "details": details,
            "fingerprint_sha256": canonical_sha256(details),
            "input_origin": {"kind": "physical", "evidence": "offline-signed-collector"},
        }

    def _full_synthetic_fixture(self, artifact_id):
        value = copy.deepcopy(self.value)
        value["collector_attestation"] = {
            "algorithm": "ed25519",
            "public_key_base64": TEST_COLLECTOR_PUBLIC_KEY,
            "key_id": "offline-test-collector",
        }
        env = self._full_environment(value)
        expected_artifact = value["artifacts"][artifact_id]
        actual_artifact = {"id": artifact_id, **copy.deepcopy(expected_artifact)}
        run_id = f"offline-{artifact_id}"
        binding = {
            "task_id": value["task_id"],
            "iteration": value["iteration"],
            "plan_revision": value["plan_revision"],
            "contract_id": value["contract_id"],
            "run_id": run_id,
            "artifact_id": artifact_id,
            "provenance_id": actual_artifact["provenance_id"],
            "environment_fingerprint": env["fingerprint_sha256"],
        }
        observation_counters = {}
        attempts = []
        cursor = 10_000_000_000
        for case in value["cases"]:
            if not case.get("authorized", True):
                continue
            observer_id = case["checkpoints"][0]["observer_id"]
            for attempt_index in range(1, case["repetitions"] + 1):
                attempt_id = f"primary-{case['id']}-{attempt_index}"
                item = attempt(
                    case,
                    attempt_id=attempt_id,
                    start=cursor,
                    finish=cursor + 4 * NS,
                    observer_id=observer_id,
                )
                item.update(binding)
                item["attempt_index"] = attempt_index
                item["input_origin_evidence"] = env["input_origin"]["evidence"]
                for observed in [item["before"], *item["checkpoints"].values()]:
                    observed["run_id"] = run_id
                cursor = retime_attempt(item, case, cursor, observation_counters) + 10_000_000
                attempts.append(item)

        identity = {"pid": 4141, "start_ticks": 9191, "daemon_epoch": f"epoch-{artifact_id}"}
        soak_start, soak_finish, active, idle = collector_intervals(cursor + NS)
        cycle = value["budgets"]["soak"]["operation_case_cycle"]
        cases = {case["id"]: case for case in value["cases"] if case.get("authorized", True)}
        per_interval = [67, 67, 66]
        operation_log = []
        operation_index = 0
        for interval, count in zip(active, per_interval):
            spacing = (interval["end_ns"] - interval["start_ns"]) // (count + 1)
            for local_index in range(count):
                case = cases[cycle[operation_index % len(cycle)]]
                start = interval["start_ns"] + spacing * (local_index + 1)
                attempt_id = f"soak-{artifact_id}-{operation_index}"
                item = attempt(
                    case,
                    attempt_id=attempt_id,
                    start=start,
                    finish=start + 4 * NS,
                    observer_id=case["checkpoints"][0]["observer_id"],
                )
                item.update(binding)
                item["input_origin_evidence"] = env["input_origin"]["evidence"]
                for observed in [item["before"], *item["checkpoints"].values()]:
                    observed["run_id"] = run_id
                retime_attempt(item, case, start, observation_counters)
                item.update({
                    "operation_id": attempt_id,
                    "monotonic_ns": start,
                    "result": "PASS",
                    "process_identity": identity,
                })
                operation_log.append(item)
                operation_index += 1
        soak = {
            **binding,
            "started_monotonic_ns": soak_start,
            "finished_monotonic_ns": soak_finish,
            "process_identity": identity,
            "process_restarts": 0,
            "active_intervals": active,
            "idle_intervals": idle,
            "operation_log": operation_log,
        }
        return value, env, actual_artifact, attempts, soak

    @staticmethod
    def _retime_action(item, action_index, started_ns, checkpoint_id):
        completed_ns = started_ns + 1_000_000
        item["actions"][action_index]["started_monotonic_ns"] = started_ns
        item["actions"][action_index]["completed_monotonic_ns"] = completed_ns
        observed = item["checkpoints"][checkpoint_id]
        captured = completed_ns + 1_000_000
        observed["captured_monotonic_ns"] = captured
        observed["clock"]["observer_ns"] = captured
        observed["clock"]["collector_ns"] = captured
        item["finished_monotonic_ns"] = completed_ns + 10_000_000
        for mapping in item["clock_mappings"].values():
            mapping["calibration"]["after"] = {
                "observer_ns": item["finished_monotonic_ns"],
                "collector_ns": item["finished_monotonic_ns"],
            }
        sign_attempt(item)

    @staticmethod
    def _shift_attempt_timeline(item, delta):
        item["started_monotonic_ns"] += delta
        item["finished_monotonic_ns"] += delta
        if "monotonic_ns" in item:
            item["monotonic_ns"] += delta
        for action in item["actions"]:
            action["started_monotonic_ns"] += delta
            action["completed_monotonic_ns"] += delta
        for observed in [item["before"], *item["checkpoints"].values()]:
            observed["captured_monotonic_ns"] += delta
            observed["clock"]["observer_ns"] += delta
            observed["clock"]["collector_ns"] += delta
            confirmation = observed.get("completion_confirmation")
            if isinstance(confirmation, dict):
                confirmation["earliest_monotonic_ns"] += delta
                confirmation["latest_monotonic_ns"] += delta
        for mapping in item["clock_mappings"].values():
            for boundary in ("before", "after"):
                mapping["calibration"][boundary]["observer_ns"] += delta
                mapping["calibration"][boundary]["collector_ns"] += delta
        sign_attempt(item)

    @classmethod
    def _retime_full_undo_confirmation(cls, attempts, soak, elapsed_ms, *, interval_width_ms=0):
        target_index = next(
            index for index, item in enumerate(attempts)
            if item["case_id"] == "punto-undo-within-2500ms"
        )
        item = attempts[target_index]
        old_finish = item["finished_monotonic_ns"]
        space_completed = item["actions"][1]["completed_monotonic_ns"]
        earliest = space_completed + 300_000_000
        latest = earliest + interval_width_ms * 1_000_000
        correction = item["checkpoints"]["after-automatic-correction"]
        correction_captured = latest + 1_000_000
        correction["captured_monotonic_ns"] = correction_captured
        correction["clock"]["observer_ns"] = correction_captured
        correction["clock"]["collector_ns"] = correction_captured
        undo_started = earliest + elapsed_ms * 1_000_000
        cls._retime_action(item, 2, undo_started, "after-punto-undo")
        add_completion_confirmation(
            item,
            "after-automatic-correction",
            1,
            earliest=earliest,
            latest=latest,
        )
        sign_attempt(item)
        delta = item["finished_monotonic_ns"] - old_finish
        for following in attempts[target_index + 1:]:
            cls._shift_attempt_timeline(following, delta)
        soak["started_monotonic_ns"] += delta
        soak["finished_monotonic_ns"] += delta
        for interval in [*soak["active_intervals"], *soak["idle_intervals"]]:
            interval["start_ns"] += delta
            interval["end_ns"] += delta
        for operation in soak["operation_log"]:
            cls._shift_attempt_timeline(operation, delta)
        return item

    @staticmethod
    def _attempt_for(attempts, case_id):
        return next(item for item in attempts if item["case_id"] == case_id)

    def test_full_repository_contract_fixture_is_exact_for_both_artifacts(self):
        authorized = [case for case in self.value["cases"] if case.get("authorized", True)]
        expected_ids = [case["id"] for case in authorized]
        repetition_pairs = [(case["id"], case["repetitions"]) for case in authorized]
        self.assertEqual(R10_AUTHORIZED_CASES, len(authorized))
        self.assertEqual(R10_PRIMARY_ATTEMPTS, sum(case["repetitions"] for case in authorized))
        self.assertEqual(R10_AUTHORIZED_REPETITIONS_SHA256, canonical_sha256(repetition_pairs))
        for artifact_id in ("baseline_2_8_5", "candidate_2_8_14"):
            with self.subTest(artifact_id=artifact_id):
                value, _, actual_artifact, attempts, soak = self._full_synthetic_fixture(artifact_id)
                self.assertEqual(expected_ids, [case["id"] for case in value["cases"] if case.get("authorized", True)])
                self.assertEqual(R10_PRIMARY_ATTEMPTS, len(attempts))
                self.assertEqual(200, len(soak["operation_log"]))
                self.assertEqual(artifact_id, actual_artifact["id"])
                report = evaluate_run(value, self._full_environment(value), actual_artifact, attempts, soak)
                self.assertEqual("PASS", report["status"], report)
                for case in value["cases"]:
                    if not case.get("authorized", True):
                        self.assertEqual("NOT_RUN_AUTHORIZATION", report["case_results"][case["id"]])

    def test_full_packet_detects_manual_unknown_word_regression(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        failed = self._attempt_for(attempts, "manual-unknown-word-inversion")
        failed["checkpoints"]["after-manual-inversion"]["state"]["text"] = "qzx"
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("FAIL_PRODUCT", report["status"])
        self.assertIn(failed["attempt_id"], report["confirmed_product_failures"])

    def test_full_packet_detects_no_active_word_and_learning_separator_regressions(self):
        for case_id, checkpoint_id in (
            ("safe-no-active-word-then-recovery", "after-empty-word-rejection"),
            ("isolated-learning-reset", "after-automatic-correction"),
        ):
            with self.subTest(case_id=case_id):
                value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
                failed = self._attempt_for(attempts, case_id)
                failed["checkpoints"][checkpoint_id]["state"]["text"] = "wrong-without-separator"
                report = evaluate_run(value, env, actual_artifact, attempts, soak)
                expected_status = "INVALID_OBSERVATION" if case_id == "isolated-learning-reset" else "FAIL_PRODUCT"
                self.assertEqual(expected_status, report["status"])
                self.assertIn(failed["attempt_id"], report["confirmed_product_failures"])

    def test_full_packet_detects_delayed_undo_event_relation(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        item = self._attempt_for(attempts, "punto-undo-within-2500ms")
        confirmation = item["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        delayed = confirmation["latest_monotonic_ns"] + 2_501_000_000
        self._retime_action(item, 2, delayed, "after-punto-undo")
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("temporal_constraint_exceeded" in reason for reason in report["reasons"]))

    def test_full_packet_accepts_delayed_correction_anchor_for_both_artifacts(self):
        for artifact_id in ("baseline_2_8_5", "candidate_2_8_14"):
            with self.subTest(artifact_id=artifact_id):
                value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture(artifact_id)
                item = self._retime_full_undo_confirmation(attempts, soak, 2300)
                space_completed = item["actions"][1]["completed_monotonic_ns"]
                confirmation = item["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
                undo_started = item["actions"][2]["started_monotonic_ns"]
                self.assertGreater(undo_started - space_completed, 2_500_000_000)
                self.assertEqual(2_300_000_000, undo_started - confirmation["earliest_monotonic_ns"])
                report = evaluate_run(value, env, actual_artifact, attempts, soak)
                self.assertEqual("PASS", report["status"], report)

    def test_full_packet_correction_boundary_straddle_and_missing_confirmation(self):
        for mode, elapsed_ms, width_ms, expected_status, expected_reason in (
            ("boundary", 2500, 0, "PASS", None),
            ("exceeded", 2501, 0, "INVALID_OBSERVATION", "temporal_constraint_exceeded"),
            ("straddle", 2501, 2, "INVALID_OBSERVATION", "temporal_constraint_indeterminate"),
        ):
            with self.subTest(mode=mode):
                value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
                self._retime_full_undo_confirmation(
                    attempts,
                    soak,
                    elapsed_ms,
                    interval_width_ms=width_ms,
                )
                report = evaluate_run(value, env, actual_artifact, attempts, soak)
                self.assertEqual(expected_status, report["status"], report)
                if expected_reason:
                    self.assertTrue(any(expected_reason in reason for reason in report["reasons"]))

        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        missing = self._attempt_for(attempts, "punto-undo-within-2500ms")
        del missing["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"], report)
        self.assertTrue(any("missing_completion_confirmation" in reason for reason in report["reasons"]))

    def test_full_packet_rejects_package_wide_confirmation_replay(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        matching = [
            item for item in attempts
            if item["case_id"] == "punto-undo-within-2500ms"
        ]
        first = matching[0]["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        replayed = matching[1]["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        replayed["confirmation_id"] = first["confirmation_id"]
        sign_attempt(matching[1])

        report = evaluate_run(value, env, actual_artifact, attempts, soak)

        self.assertEqual("INVALID_OBSERVATION", report["status"], report)
        self.assertTrue(any("reused_completion_confirmation" in reason for reason in report["reasons"]))

        case = correction_completion_case()
        small = contract()
        small["cases"] = [case]
        small["budgets"]["soak"]["operation_case_cycle"] = [case["id"]]
        primary = attempt(case, attempt_id="primary-confirmation")
        primary_id = primary["checkpoints"]["after-automatic-correction"]["completion_confirmation"]["confirmation_id"]
        correction_soak = complete_soak(case)
        soak_confirmation = correction_soak["operation_log"][0]["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        soak_confirmation["confirmation_id"] = primary_id
        sign_attempt(correction_soak["operation_log"][0])
        cross_lane = evaluate(small, [primary], soak=correction_soak)
        self.assertEqual("INVALID_OBSERVATION", cross_lane["status"], cross_lane)
        self.assertTrue(any(
            reason.startswith("soak:soak-0:reused_completion_confirmation")
            for reason in cross_lane["reasons"]
        ))

        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        matching = [
            item for item in attempts
            if item["case_id"] == "punto-undo-within-2500ms"
        ]
        first = matching[0]["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        second = matching[1]["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        first["confirmation_id"] = second["confirmation_id"]
        first["signature_base64"] = "invalid"
        non_poisoning = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", non_poisoning["status"], non_poisoning)
        self.assertTrue(any("invalid_completion_confirmation_signature" in reason for reason in non_poisoning["reasons"]))
        self.assertFalse(any("reused_completion_confirmation" in reason for reason in non_poisoning["reasons"]))

        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        matching = [
            item for item in attempts
            if item["case_id"] == "punto-undo-within-2500ms"
        ]
        first_snapshot = matching[0]["checkpoints"]["after-automatic-correction"]
        first_confirmation = first_snapshot["completion_confirmation"]
        second_confirmation = matching[1]["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        first_snapshot["state"]["text"] = "wrong correction "
        first_confirmation["confirmation_id"] = second_confirmation["confirmation_id"]
        first_confirmation["result_state_sha256"] = canonical_sha256(first_snapshot["state"])
        sign_attempt(matching[0])

        result_unconfirmed_non_poisoning = evaluate_run(value, env, actual_artifact, attempts, soak)

        self.assertEqual(
            "INVALID_OBSERVATION",
            result_unconfirmed_non_poisoning["status"],
            result_unconfirmed_non_poisoning,
        )
        self.assertTrue(any(
            "completion_confirmation_result_unconfirmed" in reason
            for reason in result_unconfirmed_non_poisoning["reasons"]
        ))
        self.assertFalse(any(
            "reused_completion_confirmation" in reason
            for reason in result_unconfirmed_non_poisoning["reasons"]
        ))
        self.assertIn(
            matching[0]["attempt_id"],
            result_unconfirmed_non_poisoning["confirmed_product_failures"],
        )

    def test_full_packet_detects_delayed_rapid_tail_event_relation(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        item = self._attempt_for(attempts, "rapid-tail-preserved")
        delayed = item["actions"][1]["completed_monotonic_ns"] + 101_000_000
        self._retime_action(item, 2, delayed, "after-rapid-tail")
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("temporal_constraint_exceeded" in reason for reason in report["reasons"]))

    def test_full_packet_detects_early_expiry_action_and_late_intermediate_checkpoint(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        expiry = self._attempt_for(attempts, "punto-undo-expired-without-input")
        confirmation = expiry["checkpoints"]["after-automatic-correction"]["completion_confirmation"]
        correction_completed = confirmation["latest_monotonic_ns"]
        expiry["actions"][2]["started_monotonic_ns"] = correction_completed + 1_000_000
        expiry["actions"][2]["completed_monotonic_ns"] = correction_completed + 2_000_000
        self._retime_action(expiry, 3, correction_completed + 2_500_000_000, "after-expired-ctrl-z")
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("temporal_constraint_too_early" in reason for reason in report["reasons"]))

        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        rapid = self._attempt_for(attempts, "rapid-tail-preserved")
        observed = rapid["checkpoints"]["after-space-correction"]
        captured = rapid["actions"][2]["started_monotonic_ns"] + 1
        observed["captured_monotonic_ns"] = captured
        observed["clock"]["observer_ns"] = captured
        observed["clock"]["collector_ns"] = captured
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("checkpoint_after_later_action" in reason for reason in report["reasons"]))

    def test_full_packet_detects_primary_and_soak_budget_overrun(self):
        for lane in ("primary", "soak"):
            with self.subTest(lane=lane):
                value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
                item = attempts[0] if lane == "primary" else soak["operation_log"][0]
                item["finished_monotonic_ns"] = item["started_monotonic_ns"] + 5 * NS + 1
                for mapping in item["clock_mappings"].values():
                    mapping["calibration"]["after"] = {
                        "observer_ns": item["finished_monotonic_ns"],
                        "collector_ns": item["finished_monotonic_ns"],
                    }
                sign_attempt(item)
                report = evaluate_run(value, env, actual_artifact, attempts, soak)
                self.assertEqual("INVALID_OBSERVATION", report["status"])
                self.assertTrue(any("oracle_budget_exceeded" in reason for reason in report["reasons"]))

    def test_full_packet_retains_primary_failure_before_missing_tail(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        failed = attempts[0]
        first_checkpoint = failed["checkpoints"][next(iter(failed["checkpoints"]))]
        first_checkpoint["state"]["text"] = "wrong"
        failed["actions"].pop()
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertEqual("FAIL_PRODUCT", report["case_results"][failed["case_id"]])
        self.assertIn(failed["attempt_id"], report["confirmed_product_failures"])

    def test_full_packet_retains_early_failure_before_budget_overrun(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        failed = attempts[0]
        first_checkpoint = failed["checkpoints"][next(iter(failed["checkpoints"]))]
        first_checkpoint["state"]["text"] = "wrong"
        failed["finished_monotonic_ns"] = failed["started_monotonic_ns"] + 5 * NS + 1
        for mapping in failed["clock_mappings"].values():
            mapping["calibration"]["after"] = {
                "observer_ns": failed["finished_monotonic_ns"],
                "collector_ns": failed["finished_monotonic_ns"],
            }
        sign_attempt(failed)
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertIn(failed["attempt_id"], report["confirmed_product_failures"])

    def test_full_packet_untrusted_prefixes_never_publish_failure(self):
        for mutation in ("signature", "binding"):
            with self.subTest(mutation=mutation):
                value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
                item = self._attempt_for(attempts, "manual-unknown-word-inversion")
                item["checkpoints"]["after-manual-inversion"]["state"]["text"] = "wrong"
                if mutation == "signature":
                    item["actions"][1]["signature_base64"] = "invalid"
                else:
                    item["artifact_id"] = "foreign"
                report = evaluate_run(value, env, actual_artifact, attempts, soak)
                self.assertEqual("INVALID_OBSERVATION", report["status"])
                self.assertNotIn(item["attempt_id"], report["confirmed_product_failures"])

    def test_full_packet_retains_soak_failure_before_truncated_log(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        failed = soak["operation_log"][0]
        first_checkpoint = failed["checkpoints"][next(iter(failed["checkpoints"]))]
        first_checkpoint["state"]["text"] = "wrong"
        failed["result"] = "FAIL_PRODUCT"
        soak["operation_log"].pop()
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertIn(failed["attempt_id"], report["confirmed_product_failures"])

    def test_full_packet_rejects_soak_time_moving_backward_after_primary(self):
        value, env, actual_artifact, attempts, soak = self._full_synthetic_fixture("candidate_2_8_14")
        delta = 1 - soak["started_monotonic_ns"]
        soak["started_monotonic_ns"] += delta
        soak["finished_monotonic_ns"] += delta
        for interval in [*soak["active_intervals"], *soak["idle_intervals"]]:
            interval["start_ns"] += delta
            interval["end_ns"] += delta
        for operation in soak["operation_log"]:
            operation["started_monotonic_ns"] += delta
            operation["finished_monotonic_ns"] += delta
            operation["monotonic_ns"] += delta
            for action in operation["actions"]:
                action["started_monotonic_ns"] += delta
                action["completed_monotonic_ns"] += delta
            for observed in [operation["before"], *operation["checkpoints"].values()]:
                observed["captured_monotonic_ns"] += delta
                observed["clock"]["observer_ns"] += delta
                observed["clock"]["collector_ns"] += delta
            for mapping in operation["clock_mappings"].values():
                for point in ("before", "after"):
                    mapping["calibration"][point]["observer_ns"] += delta
                    mapping["calibration"][point]["collector_ns"] += delta
            sign_attempt(operation)
        report = evaluate_run(value, env, actual_artifact, attempts, soak)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertTrue(any("non_monotonic_observer_time" in reason for reason in report["reasons"]))

    def test_repository_contract_synthetic_set_and_required_mutations(self):
        fixture = self._synthetic_fixture()

        def run(current):
            return evaluate_run(*current)

        self.assertEqual("PASS", run(fixture)["status"])

        current = copy.deepcopy(fixture)
        del current[3][0]["checkpoints"]["after-space-correction"]
        self.assertEqual("INVALID_OBSERVATION", run(current)["status"])

        current = copy.deepcopy(fixture)
        failed = current[3][0]
        failed["checkpoints"]["after-space-correction"]["state"]["text"] = "wrong"
        del failed["checkpoints"]["after-rapid-tail"]
        report = run(current)
        self.assertEqual("INVALID_OBSERVATION", report["status"])
        self.assertIn(failed["attempt_id"], report["confirmed_product_failures"])

        current = copy.deepcopy(fixture)
        current[0]["cases"][0]["repetitions"] = 2
        self.assertEqual("INVALID_OBSERVATION", run(current)["status"])

        current = copy.deepcopy(fixture)
        current[3][0]["artifact_id"] = "foreign"
        self.assertEqual("INVALID_OBSERVATION", run(current)["status"])

        current = copy.deepcopy(fixture)
        current[0]["cases"][0]["initial_state"] = {}
        current[0]["cases"][0]["expected_state"] = {}
        current[0]["cases"][0]["checkpoints"][0]["expected_state"] = {}
        self.assertEqual("INVALID_OBSERVATION", run(current)["status"])

        current = copy.deepcopy(fixture)
        del current[4]["operation_log"][0]["result"]
        self.assertEqual("INVALID_OBSERVATION", run(current)["status"])

        current = copy.deepcopy(fixture)
        failed = current[4]["operation_log"][1]
        failed["checkpoints"]["after-operation"]["state"]["text"] = "wrong"
        failed["result"] = "FAIL_PRODUCT"
        self.assertEqual("FAIL_PRODUCT", run(current)["status"])

        current = copy.deepcopy(fixture)
        foreign = current[3][1]
        foreign["run_id"] = "run-2"
        for snapshot_value in [foreign["before"], *foreign["checkpoints"].values()]:
            snapshot_value["run_id"] = "run-2"
        self.assertEqual("INVALID_OBSERVATION", run(current)["status"])

        current = copy.deepcopy(fixture)
        reused = current[3][0]
        reused["checkpoints"]["after-space-correction"]["observation_id"] = reused["before"]["observation_id"]
        self.assertEqual("INVALID_OBSERVATION", run(current)["status"])


if __name__ == "__main__":
    unittest.main()
