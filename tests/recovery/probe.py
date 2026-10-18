#!/usr/bin/env python3
"""Validate Punto recovery evidence without controlling Punto or the desktop."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import time
import uuid
from collections import Counter
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


REQUIRED_STATUSES = {
    "PASS",
    "FAIL_PRODUCT",
    "INVALID_OBSERVATION",
    "BLOCKED_IDENTITY",
    "BLOCKED_ENVIRONMENT",
    "BLOCKED_INPUT_ORIGIN",
    "NOT_RUN_AUTHORIZATION",
    "ABORTED_SAFETY",
}
PLACEHOLDERS = {"scenario_defined", "all", "both", "unchanged_or_documented"}
TEXT_SURFACES = {
    "gtk_entry", "gtk_text_view", "chromium_input", "chromium_textarea", "scratch_terminal",
}
SUPPORTED_SURFACES = TEXT_SURFACES | {"isolated_profile", "tray", "cli"}
ORACLE_KEYS = {
    "gtk_entry": {"text", "selection", "focus_target", "layout", "following_text"},
    "gtk_text_view": {"text", "selection", "focus_target", "layout"},
    "chromium_input": {"text", "selection", "focus_target", "layout", "following_text"},
    "chromium_textarea": {"text", "selection", "focus_target", "layout", "sentinel_input"},
    "scratch_terminal": {"text", "selection", "focus_target", "layout", "command_executed"},
    "isolated_profile": {"text", "learning_count", "profile", "reset_confirmed"},
    "tray": {"window", "auto_correction", "manual_operations_available"},
    "cli": {"exit_code", "reports_backend", "reports_frontend"},
}
CONTRACT_SCHEMA = {
    "type": "object",
    "required": [
        "contract_id", "task_id", "iteration", "plan_revision", "statuses",
        "supported_environment", "artifacts", "budgets", "safety", "cases",
        "collector_attestation",
    ],
    "properties": {
        "contract_id": {"type": "string", "minLength": 1},
        "task_id": {"type": "string", "minLength": 1},
        "iteration": {"type": "integer", "minimum": 0},
        "plan_revision": {"type": "integer", "minimum": 1},
        "statuses": {"type": "array", "items": {"type": "string"}, "uniqueItems": True},
        "supported_environment": {"type": "object", "required": ["expected", "required_present"]},
        "artifacts": {"type": "object", "minProperties": 1},
        "budgets": {"type": "object", "required": ["oracle_seconds", "soak"]},
        "safety": {"type": "object", "required": ["abort_on"]},
        "cases": {"type": "array", "minItems": 1},
        "collector_attestation": {
            "type": "object",
            "required": ["algorithm", "public_key_base64"],
        },
    },
}


class ObservationProtocol:
    """Mechanical freshness, attempt binding and clock mapping for observers."""

    def __init__(
        self,
        observer_id: str = "gtk",
        observer_domain: str = "collector_monotonic_ns",
        clock: Any = time.monotonic_ns,
    ) -> None:
        if not observer_id or not observer_domain:
            raise ValueError("observer_id and observer_domain are required")
        self.observer_id = observer_id
        self.observer_instance_id = f"{observer_id}-{uuid.uuid4()}"
        self.observer_domain = observer_domain
        self.clock = clock
        self.observation_id = 0
        self.state_generation = 0
        self.run_id: str | None = None
        self.case_id: str | None = None
        self.attempt_id: str | None = None
        self.clock_mapping: dict[str, Any] | None = None

    @property
    def active(self) -> bool:
        return self.run_id is not None

    def ensure_prepare_allowed(self) -> None:
        if self.active:
            raise RuntimeError("active case; preparation is forbidden")

    def record_state_change(self) -> None:
        self.state_generation += 1

    def arm(
        self,
        run_id: str,
        case_id: str,
        attempt_id: str,
        clock_mapping: dict[str, Any] | None = None,
    ) -> None:
        if self.active:
            raise RuntimeError("a case is already active")
        if not run_id or not case_id or not attempt_id:
            raise ValueError("run_id, case_id and attempt_id are required")
        if clock_mapping is None:
            raise ValueError("collector-confirmed clock mapping is required")
        mapping = clock_mapping
        if (
            mapping.get("observer_id") != self.observer_id
            or
            mapping.get("observer_instance_id") != self.observer_instance_id
            or
            mapping.get("observer_domain") != self.observer_domain
            or mapping.get("collector_domain") != "collector_monotonic_ns"
            or not isinstance(mapping.get("offset_ns"), int)
            or not mapping.get("mapping_id")
            or not isinstance(mapping.get("calibration"), dict)
            or mapping["calibration"].get("source") != "collector_handshake"
            or not mapping["calibration"].get("calibration_id")
        ):
            raise ValueError("invalid observer clock mapping")
        self.run_id = run_id
        self.case_id = case_id
        self.attempt_id = attempt_id
        self.clock_mapping = dict(mapping)

    def snapshot(self, state: dict[str, Any]) -> dict[str, Any]:
        if not self.active:
            raise RuntimeError("no active case")
        self.observation_id += 1
        observer_ns = self.clock()
        assert self.clock_mapping is not None
        collector_ns = observer_ns + self.clock_mapping["offset_ns"]
        return {
            "observation_id": self.observation_id,
            "observer_id": self.observer_id,
            "observer_instance_id": self.observer_instance_id,
            "state_generation": self.state_generation,
            "captured_monotonic_ns": collector_ns,
            "run_id": self.run_id,
            "case_id": self.case_id,
            "attempt_id": self.attempt_id,
            "clock": {
                **self.clock_mapping,
                "observer_ns": observer_ns,
                "collector_ns": collector_ns,
            },
            "state": state,
        }

    def complete(self) -> None:
        if not self.active:
            raise RuntimeError("no active case")
        self.run_id = None
        self.case_id = None
        self.attempt_id = None
        self.clock_mapping = None


def canonical_sha256(value: Any) -> str:
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _calibration_payload(mapping: dict[str, Any], attempt: dict[str, Any]) -> bytes:
    calibration = mapping.get("calibration", {})
    value = {
        "version": 1,
        "mapping_id": mapping.get("mapping_id"),
        "observer_id": mapping.get("observer_id"),
        "observer_instance_id": mapping.get("observer_instance_id"),
        "observer_domain": mapping.get("observer_domain"),
        "collector_domain": mapping.get("collector_domain"),
        "offset_ns": mapping.get("offset_ns"),
        "calibration_id": calibration.get("calibration_id"),
        "source": calibration.get("source"),
        "before": calibration.get("before"),
        "after": calibration.get("after"),
        "binding": {
            field: attempt.get(field)
            for field in (
                "task_id", "iteration", "plan_revision", "contract_id", "run_id",
                "case_id", "attempt_id", "artifact_id", "provenance_id",
                "environment_fingerprint",
            )
        },
    }
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _action_payload(action: dict[str, Any], attempt: dict[str, Any]) -> bytes:
    value = {
        "version": 1,
        "collector_event_id": action.get("collector_event_id"),
        "source": action.get("source"),
        "action_index": action.get("action_index"),
        "action": action.get("action"),
        "started_monotonic_ns": action.get("started_monotonic_ns"),
        "completed_monotonic_ns": action.get("completed_monotonic_ns"),
        "binding": {
            field: attempt.get(field)
            for field in (
                "task_id", "iteration", "plan_revision", "contract_id", "run_id",
                "case_id", "attempt_id", "artifact_id", "provenance_id",
                "environment_fingerprint",
            )
        },
    }
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _completion_confirmation_payload(confirmation: dict[str, Any]) -> bytes:
    binding_fields = (
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
        "binding": {field: confirmation.get(field) for field in binding_fields},
    }
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _verify_signature(payload: bytes, signature_base64: Any, trust: dict[str, Any]) -> bool:
    if trust.get("algorithm") != "ed25519":
        return False
    try:
        public_key = Ed25519PublicKey.from_public_bytes(base64.b64decode(trust["public_key_base64"], validate=True))
        signature = base64.b64decode(signature_base64, validate=True)
        public_key.verify(signature, payload)
    except (KeyError, ValueError, TypeError, InvalidSignature):
        return False
    return True


def _calibration_signature_error(
    mapping: dict[str, Any], attempt: dict[str, Any], trust: dict[str, Any]
) -> str | None:
    if not _verify_signature(
        _calibration_payload(mapping, attempt),
        mapping.get("calibration", {}).get("signature_base64"),
        trust,
    ):
        return "signature_unconfirmed"
    return None


def _walk_strings(value: Any):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from _walk_strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from _walk_strings(item)


def _contract_errors(contract: dict[str, Any]) -> list[str]:
    errors = sorted(Draft202012Validator(CONTRACT_SCHEMA).iter_errors(contract), key=lambda item: list(item.path))
    reasons = [f"invalid_contract:{'.'.join(map(str, item.path)) or '$'}:{item.message}" for item in errors]
    if errors:
        return reasons
    if set(contract["statuses"]) != REQUIRED_STATUSES:
        reasons.append("invalid_contract:statuses")
    trust = contract["collector_attestation"]
    try:
        key_bytes = base64.b64decode(trust.get("public_key_base64", ""), validate=True)
    except (ValueError, TypeError):
        key_bytes = b""
    if trust.get("algorithm") != "ed25519" or len(key_bytes) != 32:
        reasons.append("invalid_contract:collector_attestation")
    case_ids = [case.get("id") for case in contract["cases"]]
    if len(case_ids) != len(set(case_ids)) or any(not value for value in case_ids):
        reasons.append("invalid_contract:case_ids")
    for case in contract["cases"]:
        authorized = case.get("authorized", True)
        if authorized:
            for field in ("surface", "direction", "steps", "initial_state", "expected_state", "checkpoints"):
                if field not in case:
                    reasons.append(f"invalid_contract:{case.get('id')}:{field}")
            if not isinstance(case.get("repetitions"), int) or case.get("repetitions", 0) <= 0:
                reasons.append(f"invalid_contract:{case.get('id')}:repetitions")
            steps = case.get("steps")
            if not isinstance(steps, list) or not steps or any(not isinstance(step, str) or not step for step in steps):
                reasons.append(f"invalid_contract:{case.get('id')}:steps")
            for field in ("initial_state", "expected_state"):
                if not isinstance(case.get(field), dict) or not case.get(field):
                    reasons.append(f"invalid_contract:{case.get('id')}:empty_oracle:{field}")
                else:
                    reasons.extend(_oracle_errors(case, case[field], field))
            checkpoints = case.get("checkpoints")
            if not isinstance(checkpoints, list) or not checkpoints:
                reasons.append(f"invalid_contract:{case.get('id')}:checkpoints")
            else:
                checkpoint_ids = [item.get("id") for item in checkpoints if isinstance(item, dict)]
                checkpoints_by_id = {
                    item["id"]: item
                    for item in checkpoints
                    if isinstance(item, dict) and isinstance(item.get("id"), str)
                }
                checkpoint_action_indexes: list[int] = []
                if len(checkpoint_ids) != len(checkpoints) or len(checkpoint_ids) != len(set(checkpoint_ids)) or any(not item for item in checkpoint_ids):
                    reasons.append(f"invalid_contract:{case.get('id')}:checkpoint_ids")
                for checkpoint in checkpoints:
                    if not isinstance(checkpoint, dict):
                        continue
                    checkpoint_id = checkpoint.get("id")
                    for field in ("after_action", "observer_id"):
                        if not isinstance(checkpoint.get(field), str) or not checkpoint.get(field):
                            reasons.append(f"invalid_contract:{case.get('id')}:{checkpoint_id}:{field}")
                    action_index = checkpoint.get("action_index")
                    actions = list(steps or [])
                    if case.get("next_input"):
                        actions.append(f"physically_type:{case['next_input'].get('symbol')}")
                    if (
                        not isinstance(action_index, int)
                        or action_index < 0
                        or action_index >= len(actions)
                        or actions[action_index] != checkpoint.get("after_action")
                    ):
                        reasons.append(f"invalid_contract:{case.get('id')}:{checkpoint_id}:action_binding")
                    else:
                        checkpoint_action_indexes.append(action_index)
                    if not isinstance(checkpoint.get("expected_state"), dict) or not checkpoint.get("expected_state"):
                        reasons.append(f"invalid_contract:{case.get('id')}:{checkpoint_id}:empty_oracle")
                    else:
                        reasons.extend(_oracle_errors(case, checkpoint["expected_state"], f"checkpoint:{checkpoint_id}"))
                    if not isinstance(checkpoint.get("time_constraint"), dict) or not checkpoint.get("time_constraint"):
                        reasons.append(f"invalid_contract:{case.get('id')}:{checkpoint_id}:time_constraint")
                    else:
                        timing = checkpoint["time_constraint"]
                        if not any(key in timing for key in ("within_attempt", "within_ms")):
                            reasons.append(f"invalid_contract:{case.get('id')}:{checkpoint_id}:time_constraint_kind")
                        if "within_ms" in timing and (
                            not isinstance(timing["within_ms"], int) or timing["within_ms"] < 0
                        ):
                            reasons.append(f"invalid_contract:{case.get('id')}:{checkpoint_id}:within_ms")
                    confirmation = checkpoint.get("confirms_event")
                    if confirmation is not None and (
                        not isinstance(confirmation, dict)
                        or confirmation.get("kind") != "automatic_word_correction"
                        or not isinstance(confirmation.get("trigger_action_index"), int)
                        or confirmation["trigger_action_index"] < 0
                        or confirmation["trigger_action_index"] >= len(actions)
                        or not isinstance(action_index, int)
                        or confirmation["trigger_action_index"] > action_index
                    ):
                        reasons.append(
                            f"invalid_contract:{case.get('id')}:{checkpoint_id}:confirms_event"
                        )
                next_input = case.get("next_input")
                if next_input:
                    if not isinstance(next_input, dict) or not isinstance(next_input.get("symbol"), str) or not next_input.get("symbol"):
                        reasons.append(f"invalid_contract:{case.get('id')}:next_input")
                        continue
                    action = f"physically_type:{next_input.get('symbol')}"
                    matching = [item for item in checkpoints if isinstance(item, dict) and item.get("after_action") == action]
                    if len(matching) != 1:
                        reasons.append(f"invalid_contract:{case.get('id')}:next_input_checkpoint")
                    elif next_input.get("expected_state") != matching[0].get("expected_state"):
                        reasons.append(f"invalid_contract:{case.get('id')}:next_input_oracle_owner")
                    if isinstance(next_input.get("expected_state"), dict):
                        reasons.extend(_oracle_errors(case, next_input["expected_state"], "next_input"))
                    operation_checkpoints = [item for item in checkpoints if item.get("action_index") != len(steps)]
                else:
                    operation_checkpoints = checkpoints
                if checkpoint_action_indexes != sorted(set(checkpoint_action_indexes)):
                    reasons.append(f"invalid_contract:{case.get('id')}:checkpoint_action_order")
                if operation_checkpoints and operation_checkpoints[-1].get("expected_state") != case.get("expected_state"):
                    reasons.append(f"invalid_contract:{case.get('id')}:final_checkpoint_oracle")
                if "rejection_guard" in case:
                    reasons.append(f"invalid_contract:{case.get('id')}:rejection_guard")
                guard = case.get("product_guard")
                if guard is not None and (
                    not isinstance(guard, dict)
                    or guard.get("type") != "no_active_word"
                    or guard.get("source") != "EventLoop::queue_manual_word_edit"
                    or guard.get("checkpoint_id") not in checkpoint_ids
                ):
                    reasons.append(f"invalid_contract:{case.get('id')}:product_guard")
                actions = list(steps or [])
                if case.get("next_input"):
                    actions.append(f"physically_type:{case['next_input'].get('symbol')}")
                relations = case.get("temporal_constraints", [])
                relation_ids = [
                    relation.get("id")
                    for relation in relations
                    if isinstance(relation, dict)
                ]
                if len(relation_ids) != len(set(relation_ids)):
                    reasons.append(f"invalid_contract:{case.get('id')}:temporal_constraint_ids")
                for relation in relations:
                    relation_id = relation.get("id") if isinstance(relation, dict) else None
                    lower = relation.get("from_action_index") if isinstance(relation, dict) else None
                    lower_checkpoint = relation.get("from_checkpoint_id") if isinstance(relation, dict) else None
                    upper = relation.get("to_action_index") if isinstance(relation, dict) else None
                    bounds = [key for key in ("min_ms", "max_ms") if isinstance(relation, dict) and key in relation]
                    action_anchor = (
                        isinstance(lower, int)
                        and lower_checkpoint is None
                        and relation.get("from_event") == "completed"
                        and lower >= 0
                        and isinstance(upper, int)
                        and upper > lower
                    )
                    checkpoint = checkpoints_by_id.get(lower_checkpoint)
                    checkpoint_anchor = (
                        lower is None
                        and isinstance(lower_checkpoint, str)
                        and checkpoint is not None
                        and relation.get("from_event") == "confirmed_correction_completed"
                        and isinstance(checkpoint.get("confirms_event"), dict)
                        and isinstance(checkpoint.get("action_index"), int)
                        and isinstance(upper, int)
                        and upper > checkpoint.get("action_index", upper)
                    )
                    if (
                        not isinstance(relation, dict)
                        or not isinstance(relation_id, str)
                        or not relation_id
                        or not isinstance(upper, int)
                        or upper >= len(actions)
                        or not (action_anchor or checkpoint_anchor)
                        or relation.get("to_event") != "started"
                        or not bounds
                        or any(not isinstance(relation[key], int) or relation[key] < 0 for key in bounds)
                        or (
                            "min_ms" in relation
                            and "max_ms" in relation
                            and relation["min_ms"] > relation["max_ms"]
                        )
                    ):
                        reasons.append(f"invalid_contract:{case.get('id')}:temporal_constraint:{relation_id}")
        elif case.get("expected_status") != "NOT_RUN_AUTHORIZATION":
            reasons.append(f"invalid_contract:{case.get('id')}:expected_status")
    found_placeholders = sorted(PLACEHOLDERS.intersection(_walk_strings(contract)))
    reasons.extend(f"invalid_contract:placeholder:{value}" for value in found_placeholders)
    return reasons


def _oracle_errors(case: dict[str, Any], state: dict[str, Any], owner: str) -> list[str]:
    surface = case.get("surface")
    if surface not in SUPPORTED_SURFACES:
        return [f"invalid_contract:{case.get('id')}:{owner}:unsupported_surface:{surface}"]
    extra_keys = sorted(set(state) - ORACLE_KEYS[surface])
    if extra_keys:
        return [
            f"invalid_contract:{case.get('id')}:{owner}:unsupported_oracle_field:{field}"
            for field in extra_keys
        ]
    required: set[str]
    if surface in TEXT_SURFACES:
        required = {"text", "selection", "focus_target", "layout"}
    elif surface == "isolated_profile":
        required = {"text", "learning_count", "profile"}
    elif surface == "tray":
        required = {"window"}
    elif surface == "cli":
        required = {"exit_code"}
    else:
        required = set()
    reasons = [
        f"invalid_contract:{case.get('id')}:{owner}:missing_oracle:{field}"
        for field in sorted(required - set(state))
    ]
    if surface in TEXT_SURFACES:
        text = state.get("text")
        selection = state.get("selection")
        focus = state.get("focus_target")
        if not isinstance(text, str):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_text")
        if (
            not isinstance(selection, list)
            or len(selection) != 2
            or not all(isinstance(value, int) and not isinstance(value, bool) for value in selection)
            or selection[0] < 0
            or selection[0] > selection[1]
            or (isinstance(text, str) and selection[1] > len(text))
        ):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_selection")
        if focus not in {
            "gtk_entry", "gtk_text_view", "chromium_input", "chromium_textarea",
            "tab_sentinel", "scratch_terminal",
        }:
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_focus_target")
        if "following_text" in state and not isinstance(state["following_text"], str):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_following_text")
        if "sentinel_input" in state and not isinstance(state["sentinel_input"], str):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_sentinel_input")
        if "command_executed" in state and not isinstance(state["command_executed"], bool):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_command_executed")
    if "layout" in required:
        layout = state.get("layout")
        if isinstance(layout, dict) and set(layout) == {"$same_as"}:
            if not isinstance(layout["$same_as"], str) or not layout["$same_as"]:
                reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_layout_reference")
            return reasons
        layout_keys = set(layout) if isinstance(layout, dict) else set()
        for field in sorted({"gnome_source", "xkb_group", "indicator"} - layout_keys):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:missing_layout:{field}")
        for field in sorted(layout_keys - {"gnome_source", "xkb_group", "indicator"}):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:unsupported_layout_field:{field}")
        if isinstance(layout, dict) and {"gnome_source", "xkb_group", "indicator"}.issubset(layout):
            source = layout["gnome_source"]
            group = layout["xkb_group"]
            indicator = layout["indicator"]
            if source not in {"en", "ru"}:
                reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_gnome_source")
            if not isinstance(group, int) or isinstance(group, bool) or group not in {0, 1}:
                reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_xkb_group")
            if indicator not in {"en", "ru"} or indicator != source:
                reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_indicator")
    elif surface == "isolated_profile":
        if not isinstance(state.get("text"), str):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_text")
        learning_count = state.get("learning_count")
        if not isinstance(learning_count, int) or isinstance(learning_count, bool) or learning_count < 0:
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_learning_count")
        if not isinstance(state.get("profile"), str) or not state.get("profile"):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_profile")
        if "reset_confirmed" in state and not isinstance(state["reset_confirmed"], bool):
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_reset_confirmed")
    elif surface == "tray":
        if state.get("window") not in {"none", "sound_settings", "settings", "about"}:
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_window")
        for field in ("auto_correction", "manual_operations_available"):
            if field in state and not isinstance(state[field], bool):
                reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_{field}")
    elif surface == "cli":
        exit_code = state.get("exit_code")
        if owner == "initial_state":
            valid_exit = exit_code is None
        else:
            valid_exit = isinstance(exit_code, int) and not isinstance(exit_code, bool) and exit_code >= 0
        if not valid_exit:
            reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_exit_code")
        for field in ("reports_backend", "reports_frontend"):
            if field in state and not isinstance(state[field], bool):
                reasons.append(f"invalid_contract:{case.get('id')}:{owner}:invalid_{field}")
    return reasons


def _report(
    status: str,
    reasons: list[str],
    *,
    first_failure: str | None = None,
    product_failures: list[str] | None = None,
    case_results: dict[str, str] | None = None,
    soak_result: str = "NOT_EVALUATED",
) -> dict[str, Any]:
    return {
        "status": status,
        "reasons": reasons,
        "execution_authority": "none",
        "first_failure_attempt_id": first_failure,
        "confirmed_product_failures": product_failures or [],
        "case_results": case_results or {},
        "soak_result": soak_result,
    }


def _path_get(value: Any, dotted: str) -> Any:
    current = value
    for part in dotted.split("."):
        if not isinstance(current, dict) or part not in current:
            raise KeyError(dotted)
        current = current[part]
    return current


def _state_mismatches(expected: Any, actual: Any, context: dict[str, Any], path: str = "state") -> list[str]:
    if isinstance(expected, dict) and set(expected) == {"$same_as"}:
        try:
            reference = _path_get(context, expected["$same_as"])
        except KeyError:
            return [f"{path}:missing_reference:{expected['$same_as']}"]
        return [] if actual == reference else [f"{path}:not_unchanged"]
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return [f"{path}:wrong_type"]
        reasons: list[str] = []
        for key, value in expected.items():
            if key not in actual:
                reasons.append(f"{path}.{key}:missing")
            else:
                reasons.extend(_state_mismatches(value, actual[key], context, f"{path}.{key}"))
        return reasons
    return [] if actual == expected else [f"{path}:mismatch"]


def _environment_error(contract: dict[str, Any], environment: dict[str, Any]) -> str | None:
    details = environment.get("details")
    if not isinstance(details, dict):
        return "missing_environment_details"
    for field in contract["supported_environment"]["required_present"]:
        if not details.get(field):
            return f"missing_environment_field:{field}"
    for field, expected in contract["supported_environment"]["expected"].items():
        if details.get(field) != expected:
            return f"unsupported_environment:{field}"
    if environment.get("fingerprint_sha256") != canonical_sha256(details):
        return "environment_fingerprint_mismatch"
    return None


def _artifact_error(contract: dict[str, Any], artifact: dict[str, Any]) -> str | None:
    expected = contract["artifacts"].get(artifact.get("id"))
    if expected is None:
        return f"unknown_artifact:{artifact.get('id')}"
    if artifact.get("provenance_id") != expected.get("provenance_id"):
        return "artifact_provenance_id_mismatch"
    if artifact.get("provenance") != expected.get("provenance"):
        return "artifact_provenance_mismatch"
    return None


def _snapshot_errors(
    snapshot: Any, attempt: dict[str, Any], role: str, collector_trust: dict[str, Any]
) -> list[str]:
    if not isinstance(snapshot, dict):
        return [f"{role}:missing"]
    reasons: list[str] = []
    for field in (
        "observation_id", "observer_id", "observer_instance_id", "state_generation", "captured_monotonic_ns",
        "run_id", "case_id", "attempt_id", "clock", "state",
    ):
        if field not in snapshot:
            reasons.append(f"{role}:{field}:missing")
    for field in ("observation_id", "state_generation", "captured_monotonic_ns"):
        if field in snapshot and (not isinstance(snapshot[field], int) or snapshot[field] < 0):
            reasons.append(f"{role}:{field}:wrong_type")
    if "state" in snapshot and not isinstance(snapshot["state"], dict):
        reasons.append(f"{role}:state:wrong_type")
    if (
        snapshot.get("run_id") != attempt.get("run_id")
        or snapshot.get("case_id") != attempt.get("case_id")
        or snapshot.get("attempt_id") != attempt.get("attempt_id")
    ):
        reasons.append(f"{role}:binding_mismatch")
    observer_id = snapshot.get("observer_id")
    mappings = attempt.get("clock_mappings")
    instance_id = snapshot.get("observer_instance_id")
    mapping = mappings.get(instance_id) if isinstance(mappings, dict) else None
    clock = snapshot.get("clock")
    if not isinstance(mapping, dict) or not isinstance(clock, dict):
        reasons.append(f"{role}:clock_mapping:missing")
    else:
        for field in (
            "mapping_id", "observer_id", "observer_instance_id", "observer_domain",
            "collector_domain", "offset_ns",
        ):
            if clock.get(field) != mapping.get(field):
                reasons.append(f"{role}:clock_mapping:{field}:mismatch")
        if mapping.get("observer_id") != observer_id:
            reasons.append(f"{role}:clock_mapping:observer_identity_mismatch")
        expected_domain = {
            "gtk": "collector_monotonic_ns",
            "chromium": "performance_now_ns",
        }.get(observer_id)
        if expected_domain is not None and mapping.get("observer_domain") != expected_domain:
            reasons.append(f"{role}:clock_mapping:observer_domain_mismatch")
        calibration = mapping.get("calibration")
        if (
            not isinstance(calibration, dict)
            or calibration.get("source") != "collector_handshake"
            or not calibration.get("calibration_id")
        ):
            reasons.append(f"{role}:clock_mapping:calibration_missing")
        else:
            signature_error = _calibration_signature_error(mapping, attempt, collector_trust)
            if signature_error:
                reasons.append(f"{role}:clock_mapping:{signature_error}")
            before = calibration.get("before")
            after = calibration.get("after")
            if not isinstance(before, dict) or not isinstance(after, dict):
                reasons.append(f"{role}:clock_mapping:calibration_missing")
            else:
                values = (
                    before.get("observer_ns"), before.get("collector_ns"),
                    after.get("observer_ns"), after.get("collector_ns"),
                )
                if not all(isinstance(value, int) for value in values):
                    reasons.append(f"{role}:clock_mapping:calibration_wrong_type")
                else:
                    before_observer, before_collector, after_observer, after_collector = values
                    offset_ns = mapping.get("offset_ns")
                    if (
                        after_observer <= before_observer
                        or after_collector <= before_collector
                        or before_collector - before_observer != offset_ns
                        or after_collector - after_observer != offset_ns
                    ):
                        reasons.append(f"{role}:clock_mapping:calibration_unconfirmed")
                    observed_ns = clock.get("observer_ns")
                    if isinstance(observed_ns, int) and not before_observer <= observed_ns <= after_observer:
                        reasons.append(f"{role}:clock_mapping:outside_calibration")
        observer_ns = clock.get("observer_ns")
        collector_ns = clock.get("collector_ns")
        offset_ns = clock.get("offset_ns")
        if not all(isinstance(value, int) for value in (observer_ns, collector_ns, offset_ns)):
            reasons.append(f"{role}:clock_mapping:wrong_type")
        elif observer_ns + offset_ns != collector_ns or collector_ns != snapshot.get("captured_monotonic_ns"):
            reasons.append(f"{role}:clock_mapping:unconfirmed")
    return reasons


def _register_snapshot(
    snapshot: Any,
    attempt: dict[str, Any],
    role: str,
    seen_observations: set[tuple[str, str, int]],
    seen_observer_times: dict[tuple[str, str], int],
    collector_trust: dict[str, Any],
    *,
    expected_observer: str | None = None,
    previous_time: int | None = None,
) -> tuple[list[str], int | None]:
    reasons = _snapshot_errors(snapshot, attempt, role, collector_trust)
    if reasons or not isinstance(snapshot, dict):
        return reasons, None
    if expected_observer is not None and snapshot.get("observer_id") != expected_observer:
        reasons.append(f"{role}:observer_mismatch")
    key = (snapshot["run_id"], snapshot["observer_instance_id"], snapshot["observation_id"])
    prior_ids = [
        observation_id
        for run_id, observer_instance_id, observation_id in seen_observations
        if run_id == snapshot["run_id"] and observer_instance_id == snapshot["observer_instance_id"]
    ]
    if key in seen_observations:
        reasons.append(f"reused_observation:{role}:{attempt.get('attempt_id')}")
    elif prior_ids and snapshot["observation_id"] <= max(prior_ids):
        reasons.append(f"stale_observation_sequence:{role}:{attempt.get('attempt_id')}")
    captured = snapshot["captured_monotonic_ns"]
    if captured < attempt.get("started_monotonic_ns", 0) or captured > attempt.get("finished_monotonic_ns", 0):
        reasons.append(f"snapshot_outside_attempt:{role}:{attempt.get('attempt_id')}")
    if previous_time is not None and captured <= previous_time:
        reasons.append(f"stale_observer_snapshot:{role}:{attempt.get('attempt_id')}")
    observer_key = (snapshot["run_id"], snapshot["observer_instance_id"])
    if observer_key in seen_observer_times and captured <= seen_observer_times[observer_key]:
        reasons.append(f"non_monotonic_observer_time:{role}:{attempt.get('attempt_id')}")
    if not reasons:
        seen_observations.add(key)
        seen_observer_times[observer_key] = captured
    return reasons, captured


def _completion_confirmation_errors(
    snapshot: dict[str, Any],
    attempt: dict[str, Any],
    checkpoint: dict[str, Any],
    actions: list[dict[str, Any]],
    collector_trust: dict[str, Any],
    seen_confirmation_ids: set[str],
) -> tuple[list[str], tuple[int, int] | None, str | None]:
    checkpoint_id = checkpoint["id"]
    confirmation = snapshot.get("completion_confirmation")
    if not isinstance(confirmation, dict):
        return [f"missing_completion_confirmation:{attempt.get('attempt_id')}:{checkpoint_id}"], None, None

    reasons: list[str] = []
    confirmation_id = confirmation.get("confirmation_id")
    if not isinstance(confirmation_id, str) or not confirmation_id:
        reasons.append(f"invalid_completion_confirmation_id:{attempt.get('attempt_id')}:{checkpoint_id}")
    elif confirmation_id in seen_confirmation_ids:
        reasons.append(f"reused_completion_confirmation:{attempt.get('attempt_id')}:{checkpoint_id}")

    expected_binding = (
        "task_id", "iteration", "plan_revision", "contract_id", "run_id",
        "case_id", "attempt_id", "artifact_id", "provenance_id",
        "environment_fingerprint",
    )
    if any(confirmation.get(field) != attempt.get(field) for field in expected_binding):
        reasons.append(f"completion_confirmation_binding_mismatch:{attempt.get('attempt_id')}:{checkpoint_id}")
    if confirmation.get("source") != "isolated_product_observer_transition":
        reasons.append(f"completion_confirmation_source_mismatch:{attempt.get('attempt_id')}:{checkpoint_id}")
    expected_event = checkpoint["confirms_event"]
    if confirmation.get("correction_kind") != expected_event["kind"]:
        reasons.append(f"completion_confirmation_kind_mismatch:{attempt.get('attempt_id')}:{checkpoint_id}")
    if confirmation.get("checkpoint_id") != checkpoint_id:
        reasons.append(f"completion_confirmation_checkpoint_mismatch:{attempt.get('attempt_id')}:{checkpoint_id}")
    for field in ("observer_id", "observer_instance_id", "observation_id", "state_generation"):
        if confirmation.get(field) != snapshot.get(field):
            reasons.append(
                f"completion_confirmation_observer_mismatch:{attempt.get('attempt_id')}:{checkpoint_id}:{field}"
            )

    trigger_index = expected_event["trigger_action_index"]
    trigger = actions[trigger_index]
    if (
        confirmation.get("trigger_action_index") != trigger_index
        or confirmation.get("trigger_collector_event_id") != trigger.get("collector_event_id")
    ):
        reasons.append(f"completion_confirmation_trigger_mismatch:{attempt.get('attempt_id')}:{checkpoint_id}")
    if confirmation.get("result_state_sha256") != canonical_sha256(snapshot.get("state")):
        reasons.append(f"completion_confirmation_result_mismatch:{attempt.get('attempt_id')}:{checkpoint_id}")
    if not _verify_signature(
        _completion_confirmation_payload(confirmation),
        confirmation.get("signature_base64"),
        collector_trust,
    ):
        reasons.append(f"invalid_completion_confirmation_signature:{attempt.get('attempt_id')}:{checkpoint_id}")

    earliest = confirmation.get("earliest_monotonic_ns")
    latest = confirmation.get("latest_monotonic_ns")
    trigger_completed = trigger.get("completed_monotonic_ns")
    captured = snapshot.get("captured_monotonic_ns")
    if (
        not isinstance(earliest, int)
        or not isinstance(latest, int)
        or not isinstance(trigger_completed, int)
        or not isinstance(captured, int)
        or not trigger_completed <= earliest <= latest <= captured
    ):
        reasons.append(f"invalid_completion_confirmation_interval:{attempt.get('attempt_id')}:{checkpoint_id}")

    if reasons:
        return reasons, None, None
    return [], (earliest, latest), confirmation_id


def _temporal_interval_reasons(
    relation: dict[str, Any], attempt_id: Any, earliest: int, latest: int, upper: int
) -> list[str]:
    relation_id = relation["id"]
    if upper < latest:
        return [f"temporal_constraint_indeterminate:{attempt_id}:{relation_id}"]
    reasons: list[str] = []
    if "max_ms" in relation:
        maximum = relation["max_ms"] * 1_000_000
        if upper - earliest <= maximum:
            pass
        elif upper - latest > maximum:
            reasons.append(f"temporal_constraint_exceeded:{attempt_id}:{relation_id}")
        else:
            reasons.append(f"temporal_constraint_indeterminate:{attempt_id}:{relation_id}")
    if "min_ms" in relation:
        minimum = relation["min_ms"] * 1_000_000
        if upper - latest >= minimum:
            pass
        elif upper - earliest < minimum:
            reasons.append(f"temporal_constraint_too_early:{attempt_id}:{relation_id}")
        else:
            reasons.append(f"temporal_constraint_indeterminate:{attempt_id}:{relation_id}")
    return reasons


def _evaluate_operation(
    case: dict[str, Any],
    item: dict[str, Any],
    expected_binding: dict[str, Any],
    input_origin: dict[str, Any],
    seen_observations: set[tuple[str, str, int]],
    seen_observer_times: dict[tuple[str, str], int],
    seen_collector_events: set[str],
    seen_confirmation_ids: set[str],
    collector_trust: dict[str, Any],
    oracle_seconds: int,
) -> tuple[list[str], list[str], int]:
    """Evaluate one primary or soak operation through the same oracle path."""

    attempt_id = item.get("attempt_id")
    reasons: list[str] = []
    failures: list[str] = []
    for field, expected in expected_binding.items():
        if item.get(field) != expected:
            reasons.append(f"attempt_binding_mismatch:{attempt_id}:{field}")
    if not isinstance(item.get("run_id"), str) or not item.get("run_id"):
        reasons.append(f"invalid_run_id:{attempt_id}")
    if case.get("requires_physical_input"):
        if item.get("input_origin") != "physical":
            reasons.append(f"attempt_input_origin_not_physical:{attempt_id}")
        if item.get("input_origin_evidence") != input_origin.get("evidence"):
            reasons.append(f"attempt_input_origin_evidence_mismatch:{attempt_id}")
    started = item.get("started_monotonic_ns")
    finished = item.get("finished_monotonic_ns")
    if not isinstance(started, int) or not isinstance(finished, int) or started <= 0 or finished <= started:
        reasons.append(f"invalid_attempt_timestamps:{attempt_id}")
        return reasons, failures, finished if isinstance(finished, int) else 0
    if reasons:
        return reasons, failures, finished
    if finished - started > oracle_seconds * 1_000_000_000:
        reasons.append(f"oracle_budget_exceeded:{attempt_id}")

    expected_actions = list(case["steps"])
    if case.get("next_input"):
        expected_actions.append(f"physically_type:{case['next_input']['symbol']}")
    actions = item.get("actions")
    if not isinstance(actions, list):
        reasons.append(f"invalid_action_log:{attempt_id}")
        return reasons, failures, finished
    if len(actions) != len(expected_actions):
        reasons.append(f"invalid_action_log:{attempt_id}")
    action_starts: dict[int, int] = {}
    action_times: dict[int, int] = {}
    trusted_prefix_end = -1
    for action_index, expected_action in enumerate(expected_actions[:len(actions)]):
        evidence = actions[action_index]
        action_reasons: list[str] = []
        if not isinstance(evidence, dict):
            action_reasons.append(f"invalid_action_evidence:{attempt_id}:{action_index}")
            reasons.extend(action_reasons)
            continue
        if evidence.get("action_index") != action_index or evidence.get("action") != expected_action:
            action_reasons.append(f"action_binding_mismatch:{attempt_id}:{action_index}")
        event_id = evidence.get("collector_event_id")
        if (
            evidence.get("source") != "isolated_input_collector"
            or not isinstance(event_id, str)
            or not event_id
            or event_id in seen_collector_events
        ):
            action_reasons.append(f"invalid_collector_event:{attempt_id}:{action_index}")
        else:
            seen_collector_events.add(event_id)
        if not _verify_signature(
            _action_payload(evidence, item), evidence.get("signature_base64"), collector_trust
        ):
            action_reasons.append(f"invalid_collector_signature:{attempt_id}:{action_index}")
        action_started = evidence.get("started_monotonic_ns")
        completed = evidence.get("completed_monotonic_ns")
        if (
            not isinstance(action_started, int)
            or not isinstance(completed, int)
            or not started <= action_started <= completed <= finished
        ):
            action_reasons.append(f"invalid_action_timestamp:{attempt_id}:{action_index}")
        elif action_times and action_started <= max(action_times.values()):
            action_reasons.append(f"non_monotonic_action_timestamp:{attempt_id}:{action_index}")
        if not action_reasons and action_index == trusted_prefix_end + 1:
            action_starts[action_index] = action_started
            action_times[action_index] = completed
            trusted_prefix_end = action_index
        reasons.extend(action_reasons)

    relation_targets = {
        relation["id"]: relation["to_action_index"]
        for relation in case.get("temporal_constraints", [])
    }
    invalid_relation_ids = {
        relation["id"]
        for relation in case.get("temporal_constraints", [])
        if isinstance(relation.get("from_checkpoint_id"), str)
    }
    checkpoint_relations: dict[str, list[dict[str, Any]]] = {}
    for relation in case.get("temporal_constraints", []):
        checkpoint_id = relation.get("from_checkpoint_id")
        if isinstance(checkpoint_id, str):
            checkpoint_relations.setdefault(checkpoint_id, []).append(relation)
            continue
        lower = relation["from_action_index"]
        upper = relation["to_action_index"]
        if upper > trusted_prefix_end:
            continue
        relation_reasons = _temporal_interval_reasons(
            relation,
            attempt_id,
            action_times[lower],
            action_times[lower],
            action_starts[upper],
        )
        reasons.extend(relation_reasons)
        if relation_reasons:
            invalid_relation_ids.add(relation["id"])

    before = item.get("before")
    snapshot_reasons, previous_time = _register_snapshot(
        before, item, "before", seen_observations, seen_observer_times, collector_trust
    )
    reasons.extend(snapshot_reasons)
    if snapshot_reasons or not isinstance(before, dict):
        return reasons, failures, finished
    context: dict[str, Any] = {"before": before, "checkpoints": {}}
    precondition_errors = _state_mismatches(case["initial_state"], before["state"], context, "before.state")
    if precondition_errors:
        reasons.extend(f"invalid_precondition:{attempt_id}:{reason}" for reason in precondition_errors)
        return reasons, failures, finished

    evidence = item.get("checkpoints")
    if not isinstance(evidence, dict):
        reasons.append(f"missing_checkpoints:{attempt_id}")
        return reasons, failures, finished
    for index, spec in enumerate(case["checkpoints"]):
        checkpoint_id = spec["id"]
        snapshot = evidence.get(checkpoint_id)
        role = f"checkpoint:{checkpoint_id}"
        if not isinstance(snapshot, dict):
            reasons.append(f"missing_checkpoint:{attempt_id}:{checkpoint_id}")
            continue
        action_index = spec["action_index"]
        if action_index > trusted_prefix_end:
            reasons.append(f"checkpoint_untrusted_action_prefix:{attempt_id}:{checkpoint_id}")
            continue
        if any(
            relation_targets[relation_id] <= action_index
            for relation_id in invalid_relation_ids
        ):
            reasons.append(f"checkpoint_outside_valid_scenario:{attempt_id}:{checkpoint_id}")
            continue
        checkpoint_reasons, captured = _register_snapshot(
            snapshot,
            item,
            role,
            seen_observations,
            seen_observer_times,
            collector_trust,
            expected_observer=spec["observer_id"],
            previous_time=previous_time,
        )
        reasons.extend(checkpoint_reasons)
        if checkpoint_reasons or captured is None:
            continue
        if snapshot.get("after_action") != spec["after_action"]:
            reasons.append(f"checkpoint_action_mismatch:{attempt_id}:{checkpoint_id}")
            continue
        if snapshot.get("action_index") != action_index:
            reasons.append(f"checkpoint_action_index_mismatch:{attempt_id}:{checkpoint_id}")
            continue
        if snapshot.get("collector_event_id") != actions[action_index].get("collector_event_id"):
            reasons.append(f"checkpoint_collector_event_mismatch:{attempt_id}:{checkpoint_id}")
            continue
        action_time = action_times[action_index]
        if captured < action_time:
            reasons.append(f"checkpoint_before_action:{attempt_id}:{checkpoint_id}")
            continue
        if captured - started > oracle_seconds * 1_000_000_000:
            reasons.append(f"checkpoint_outside_oracle_budget:{attempt_id}:{checkpoint_id}")
            continue
        later_action_time = action_starts.get(action_index + 1)
        if later_action_time is not None and captured >= later_action_time:
            reasons.append(f"checkpoint_after_later_action:{attempt_id}:{checkpoint_id}")
            continue
        constraint = spec["time_constraint"]
        elapsed = captured - action_time
        if "within_ms" in constraint and elapsed > constraint["within_ms"] * 1_000_000:
            reasons.append(f"checkpoint_timing_exceeded:{attempt_id}:{checkpoint_id}")
        previous_time = captured
        context["checkpoints"][checkpoint_id] = snapshot
        if index == 0:
            context["after"] = snapshot
        if index == len(case["checkpoints"]) - 1:
            context["after_next_input"] = snapshot
        next_input = case.get("next_input")
        if next_input and spec["after_action"] == f"physically_type:{next_input['symbol']}":
            actual_input = item.get("next_input", {})
            if actual_input.get("origin") != "physical" or actual_input.get("symbol") != next_input["symbol"]:
                failures.append(f"{role}:wrong_physical_symbol")
        state_failures = _state_mismatches(
            spec["expected_state"], snapshot["state"], context, f"{role}.state"
        )
        failures.extend(state_failures)
        relations = checkpoint_relations.get(checkpoint_id, [])
        if "confirms_event" in spec:
            confirmation_reasons, interval, confirmation_id = _completion_confirmation_errors(
                snapshot,
                item,
                spec,
                actions,
                collector_trust,
                seen_confirmation_ids,
            )
            if state_failures:
                confirmation_reasons.append(
                    f"completion_confirmation_result_unconfirmed:{attempt_id}:{checkpoint_id}"
                )
            reasons.extend(confirmation_reasons)
            if confirmation_reasons or interval is None or confirmation_id is None:
                invalid_relation_ids.update(relation["id"] for relation in relations)
            else:
                seen_confirmation_ids.add(confirmation_id)
                earliest, latest = interval
                for relation in relations:
                    upper = relation["to_action_index"]
                    if upper > trusted_prefix_end:
                        continue
                    relation_reasons = _temporal_interval_reasons(
                        relation,
                        attempt_id,
                        earliest,
                        latest,
                        action_starts[upper],
                    )
                    reasons.extend(relation_reasons)
                    if relation_reasons:
                        invalid_relation_ids.add(relation["id"])
                    else:
                        invalid_relation_ids.discard(relation["id"])
    return reasons, failures, finished


def _intervals(
    soak: dict[str, Any], expected: dict[str, Any]
) -> tuple[list[str], list[dict[str, Any]], list[dict[str, Any]]]:
    reasons: list[str] = []
    active = soak.get("active_intervals")
    idle = soak.get("idle_intervals")
    if not isinstance(active, list) or not isinstance(idle, list):
        return ["invalid_soak:intervals"], [], []
    combined: list[tuple[int, int, str]] = []
    for kind, values in (("active", active), ("idle", idle)):
        for interval in values:
            start = interval.get("start_ns") if isinstance(interval, dict) else None
            end = interval.get("end_ns") if isinstance(interval, dict) else None
            duration = interval.get("duration_seconds") if isinstance(interval, dict) else None
            if not isinstance(start, int) or not isinstance(end, int) or end <= start:
                reasons.append(f"invalid_soak:{kind}_interval")
                continue
            if not isinstance(duration, (int, float)) or end - start != duration * 1_000_000_000:
                reasons.append(f"invalid_soak:{kind}_duration")
            combined.append((start, end, kind))
    combined.sort()
    if any(current[0] < previous[1] for previous, current in zip(combined, combined[1:])):
        reasons.append("invalid_soak:overlapping_intervals")
    if combined:
        if combined[0][0] != soak.get("started_monotonic_ns") or combined[-1][1] != soak.get("finished_monotonic_ns"):
            reasons.append("invalid_soak:interval_bounds")
        if any(current[0] != previous[1] for previous, current in zip(combined, combined[1:])):
            reasons.append("invalid_soak:interval_gaps")
        if [item[2] for item in combined] != ["active", "idle", "active", "idle", "active"]:
            reasons.append("invalid_soak:interval_order")
    if sum(item.get("duration_seconds", 0) for item in active) < expected["active_seconds"]:
        reasons.append("incomplete_soak:active_seconds")
    if [item.get("duration_seconds") for item in idle] != expected["idle_intervals_seconds"]:
        reasons.append("incomplete_soak:idle_intervals")
    return reasons, active, idle


def _evaluate_soak(
    expected: dict[str, Any],
    soak: dict[str, Any] | None,
    binding: dict[str, Any],
    cases: dict[str, dict[str, Any]],
    input_origin: dict[str, Any],
    seen_observations: set[tuple[str, str, int]],
    seen_observer_times: dict[tuple[str, str], int],
    seen_collector_events: set[str],
    seen_confirmation_ids: set[str],
    seen_attempt_ids: set[str],
    collector_trust: dict[str, Any],
    oracle_seconds: int,
) -> tuple[list[str], list[tuple[int, str, str]]]:
    if not isinstance(soak, dict):
        return ["incomplete_soak:missing"], []
    log = soak.get("operation_log")
    if not isinstance(log, list):
        return ["incomplete_soak:operation_log"], []
    reasons: list[str] = []
    failures: list[tuple[int, str, str]] = []
    if len(log) != expected["operations"]:
        reasons.append("incomplete_soak:operation_log")
    for field, value in binding.items():
        if soak.get(field) != value:
            reasons.append(f"invalid_soak:binding:{field}")
    ids = [item.get("operation_id") for item in log]
    if len(ids) != len(set(ids)) or any(not value for value in ids):
        reasons.append("invalid_soak:operation_ids")
    attempt_ids = [item.get("attempt_id") for item in log]
    if (
        len(attempt_ids) != len(set(attempt_ids))
        or any(not value for value in attempt_ids)
        or any(value in seen_attempt_ids for value in attempt_ids)
    ):
        reasons.append("invalid_soak:attempt_ids")
    seen_attempt_ids.update(value for value in attempt_ids if isinstance(value, str) and value)
    identity = soak.get("process_identity")
    if not isinstance(identity, dict) or not all(identity.get(field) is not None for field in ("pid", "start_ticks", "daemon_epoch")):
        reasons.append("invalid_soak:process_identity")
    if soak.get("process_restarts") != expected["process_restarts"]:
        reasons.append("invalid_soak:process_restarts")
    started = soak.get("started_monotonic_ns")
    finished = soak.get("finished_monotonic_ns")
    if not isinstance(started, int) or not isinstance(finished, int) or finished - started < expected["elapsed_seconds"] * 1_000_000_000:
        reasons.append("incomplete_soak:elapsed_seconds")
    interval_reasons, active, idle = _intervals(soak, expected)
    reasons.extend(interval_reasons)
    cycle = expected.get("operation_case_cycle", [])
    operation_times = [item.get("started_monotonic_ns") for item in log]
    if not all(isinstance(value, int) for value in operation_times) or operation_times != sorted(operation_times):
        reasons.append("invalid_soak:operation_timestamps")
    for index, item in enumerate(log):
        case_id = item.get("case_id")
        case = cases.get(case_id)
        if case is None:
            reasons.append(f"invalid_soak:unknown_case:{case_id}")
            continue
        if cycle and case_id != cycle[index % len(cycle)]:
            reasons.append(f"invalid_soak:operation_schedule:{index}")
        if item.get("process_identity") != identity:
            reasons.append(f"invalid_soak:process_changed:{index}")
        operation_time = item.get("started_monotonic_ns")
        operation_end = item.get("finished_monotonic_ns")
        if not isinstance(operation_time, int) or not isinstance(operation_end, int) or not any(
            interval.get("start_ns", 0) <= operation_time < operation_end <= interval.get("end_ns", 0)
            for interval in active
        ):
            reasons.append(f"invalid_soak:operation_outside_active:{index}")
        operation_reasons, operation_failures, operation_finished = _evaluate_operation(
            case, item, binding, input_origin, seen_observations, seen_observer_times,
            seen_collector_events,
            seen_confirmation_ids,
            collector_trust, oracle_seconds,
        )
        reasons.extend(f"soak:{item.get('operation_id')}:{reason}" for reason in operation_reasons)
        declared = item.get("result")
        actual = "FAIL_PRODUCT" if operation_failures else "PASS"
        if declared not in {"PASS", "FAIL_PRODUCT"}:
            reasons.append(f"invalid_soak:operation_result:{index}")
        elif declared != actual:
            reasons.append(f"invalid_soak:declared_result:{index}")
        if operation_failures:
            failures.append((operation_finished, item.get("attempt_id", f"soak-{index}"), case_id))
    for index, active_interval in enumerate(active):
        if not any(
            isinstance(item.get("started_monotonic_ns"), int)
            and active_interval.get("start_ns", 0) <= item["started_monotonic_ns"] < active_interval.get("end_ns", 0)
            for item in log
        ):
            reasons.append(f"incomplete_soak:no_operation_in_active:{index}")
    return reasons, failures


def evaluate_run(
    contract: dict[str, Any],
    environment: dict[str, Any],
    artifact: dict[str, Any],
    attempts: list[dict[str, Any]],
    soak: dict[str, Any] | None,
) -> dict[str, Any]:
    """Return one fail-closed evidence status. The result grants no execution authority."""

    contract_reasons = _contract_errors(contract)
    if contract_reasons:
        return _report("INVALID_OBSERVATION", contract_reasons)
    isolation = environment.get("isolation", {})
    if not isolation.get("confirmed") or isolation.get("affects_working_session"):
        return _report("ABORTED_SAFETY", ["isolation_not_confirmed"])
    soak_operations = soak.get("operation_log", []) if isinstance(soak, dict) else []
    safety_events = [
        event
        for item in [*attempts, *soak_operations]
        for event in item.get("safety_events", [])
        if event in contract["safety"]["abort_on"]
    ]
    if safety_events:
        return _report("ABORTED_SAFETY", [f"safety_event:{safety_events[0]}"])
    artifact_reason = _artifact_error(contract, artifact)
    if artifact_reason:
        return _report("BLOCKED_IDENTITY", [artifact_reason])
    environment_reason = _environment_error(contract, environment)
    if environment_reason:
        return _report("BLOCKED_ENVIRONMENT", [environment_reason])

    authorized_cases = {case["id"]: case for case in contract["cases"] if case.get("authorized", True)}
    unauthorized_cases = {case["id"]: case for case in contract["cases"] if not case.get("authorized", True)}
    requires_physical = any(case.get("requires_physical_input") for case in authorized_cases.values())
    input_origin = environment.get("input_origin", {})
    if requires_physical and (input_origin.get("kind") != "physical" or not input_origin.get("evidence")):
        return _report("BLOCKED_INPUT_ORIGIN", ["physical_input_origin_not_proven"])

    primary = [item for item in attempts if not item.get("diagnostic_of")]
    diagnostic = [item for item in attempts if item.get("diagnostic_of")]
    observation_reasons: list[str] = []
    product_failures: list[tuple[int, str, str]] = []
    case_results = {case_id: "NOT_RUN_AUTHORIZATION" for case_id in unauthorized_cases}
    slots: dict[tuple[str, int], dict[str, Any]] = {}
    attempt_ids: set[str] = set()
    seen_observations: set[tuple[str, str, int]] = set()
    seen_observer_times: dict[tuple[str, str], int] = {}
    seen_collector_events: set[str] = set()
    seen_confirmation_ids: set[str] = set()
    invalid_cases: set[str] = set()
    failed_cases: set[str] = set()
    expected_binding = {
        "task_id": contract["task_id"],
        "iteration": contract["iteration"],
        "plan_revision": contract["plan_revision"],
        "contract_id": contract["contract_id"],
        "artifact_id": artifact["id"],
        "provenance_id": artifact["provenance_id"],
        "environment_fingerprint": environment["fingerprint_sha256"],
    }

    run_ids = {item.get("run_id") for item in primary if item.get("run_id")}
    if len(run_ids) != 1:
        observation_reasons.append("mixed_or_missing_run_binding")
        invalid_cases.update(authorized_cases)
    package_run_id = next(iter(run_ids)) if len(run_ids) == 1 else None

    ordered_primary = sorted(
        primary,
        key=lambda item: (
            not isinstance(item.get("started_monotonic_ns"), int),
            item.get("started_monotonic_ns") if isinstance(item.get("started_monotonic_ns"), int) else 0,
        ),
    )
    for item in ordered_primary:
        attempt_id = item.get("attempt_id")
        if not isinstance(attempt_id, str) or not attempt_id or attempt_id in attempt_ids:
            observation_reasons.append(f"invalid_or_duplicate_attempt_id:{attempt_id}")
            if item.get("case_id") in authorized_cases:
                invalid_cases.add(item["case_id"])
        else:
            attempt_ids.add(attempt_id)
        case_id = item.get("case_id")
        case = authorized_cases.get(case_id)
        if case is None:
            observation_reasons.append(f"unknown_or_unauthorized_case:{case_id}")
            continue
        index = item.get("attempt_index")
        if not isinstance(index, int) or index < 1 or index > case["repetitions"]:
            observation_reasons.append(f"invalid_attempt_index:{case_id}:{index}")
            invalid_cases.add(case_id)
            continue
        slot = (case_id, index)
        if slot in slots:
            observation_reasons.append(f"duplicate_primary_attempt:{case_id}:{index}")
            invalid_cases.add(case_id)
            continue
        slots[slot] = item
        operation_reasons, operation_failures, finished = _evaluate_operation(
            case, item, expected_binding, input_origin, seen_observations,
            seen_observer_times,
            seen_collector_events,
            seen_confirmation_ids,
            contract["collector_attestation"], contract["budgets"]["oracle_seconds"],
        )
        observation_reasons.extend(operation_reasons)
        if operation_reasons:
            invalid_cases.add(case_id)
        if operation_failures:
            product_failures.append((finished, attempt_id or "unknown", case_id))
            failed_cases.add(case_id)

    for case_id, case in authorized_cases.items():
        for index in range(1, case["repetitions"] + 1):
            if (case_id, index) not in slots:
                observation_reasons.append(f"missing_attempt:{case_id}:{index}")
                invalid_cases.add(case_id)
    for case_id in authorized_cases:
        if case_id in failed_cases:
            case_results[case_id] = "FAIL_PRODUCT"
        elif case_id in invalid_cases:
            case_results[case_id] = "INVALID_OBSERVATION"
        else:
            case_results[case_id] = "PASS"
    product_failures.sort(key=lambda item: item[0])
    failure_ids = [item[1] for item in product_failures]
    first_failure = failure_ids[0] if failure_ids else None
    primary_by_id = {item.get("attempt_id"): item for item in primary}
    failures_by_id = set(failure_ids)
    failure_finished = {attempt_id: finished for finished, attempt_id, _ in product_failures}
    diagnostic_counts = Counter(item.get("diagnostic_of") for item in diagnostic)
    for item in diagnostic:
        diagnostic_id = item.get("attempt_id")
        if not isinstance(diagnostic_id, str) or not diagnostic_id or diagnostic_id in attempt_ids:
            observation_reasons.append(f"invalid_or_duplicate_attempt_id:{diagnostic_id}")
        else:
            attempt_ids.add(diagnostic_id)
    for target, count in diagnostic_counts.items():
        if target not in failures_by_id:
            observation_reasons.append(f"diagnostic_missing_failure:{target}")
        if count > contract["budgets"]["diagnostic_replays_per_first_failure"]:
            observation_reasons.append(f"diagnostic_replay_limit:{target}")
    for item in diagnostic:
        target = item.get("diagnostic_of")
        started = item.get("started_monotonic_ns")
        finished = item.get("finished_monotonic_ns")
        if (
            not isinstance(started, int)
            or not isinstance(finished, int)
            or finished <= started
            or target not in failure_finished
            or started <= failure_finished[target]
        ):
            observation_reasons.append(f"diagnostic_order_invalid:{item.get('attempt_id')}")
        if first_failure is not None and target != first_failure:
            observation_reasons.append(f"diagnostic_not_first_failure:{item.get('attempt_id')}")
        if target in primary_by_id and item.get("run_id") != primary_by_id[target].get("run_id"):
            observation_reasons.append(f"diagnostic_run_mismatch:{item.get('attempt_id')}")
        for field, expected in expected_binding.items():
            if item.get(field) != expected:
                observation_reasons.append(f"diagnostic_binding_mismatch:{item.get('attempt_id')}:{field}")
    soak_binding = {**expected_binding, "run_id": package_run_id}
    soak_reasons, soak_failures = _evaluate_soak(
        contract["budgets"]["soak"],
        soak,
        soak_binding,
        authorized_cases,
        input_origin,
        seen_observations,
        seen_observer_times,
        seen_collector_events,
        seen_confirmation_ids,
        attempt_ids,
        contract["collector_attestation"],
        contract["budgets"]["oracle_seconds"],
    )
    observation_reasons.extend(soak_reasons)
    product_failures.extend(soak_failures)
    for _, _, case_id in soak_failures:
        case_results[case_id] = "FAIL_PRODUCT"
    product_failures.sort(key=lambda item: item[0])
    failure_ids = [item[1] for item in product_failures]
    first_failure = failure_ids[0] if failure_ids else None
    soak_result = "INVALID_OBSERVATION" if soak_reasons else ("FAIL_PRODUCT" if soak_failures else "PASS")
    if observation_reasons:
        return _report(
            "INVALID_OBSERVATION", observation_reasons, first_failure=first_failure,
            product_failures=failure_ids, case_results=case_results, soak_result=soak_result,
        )
    if product_failures:
        return _report(
            "FAIL_PRODUCT",
            [f"oracle_mismatch:{item[2]}" for item in product_failures],
            first_failure=first_failure,
            product_failures=failure_ids,
            case_results=case_results,
            soak_result=soak_result,
        )
    return _report("PASS", [], case_results=case_results, soak_result=soak_result)


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract", type=Path, required=True)
    parser.add_argument("--environment", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--attempts", type=Path, required=True)
    parser.add_argument("--soak", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = evaluate_run(_read_json(args.contract), _read_json(args.environment), _read_json(args.artifact), _read_json(args.attempts), _read_json(args.soak))
    rendered = json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0 if report["status"] in {"PASS", "NOT_RUN_AUTHORIZATION"} else 2


if __name__ == "__main__":
    raise SystemExit(main())
