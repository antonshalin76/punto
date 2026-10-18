from pathlib import Path
import hashlib
import json
import subprocess
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
PROTOCOL = ROOT / "protocol" / "punto-dbus-v1.xml"
EXTENSION_PROTOCOL = (ROOT / "gnome" /
                      "punto-input-source@antonshalin76" /
                      "punto-dbus-v1.xml")
GNOME_CONTRACT = ROOT / "gnome" / "gnome46-x11-contract.json"


def method_signature(interface: ET.Element, name: str) -> tuple[str, str]:
    method = next(node for node in interface.findall("method")
                  if node.attrib["name"] == name)
    inputs = "".join(arg.attrib["type"] for arg in method.findall("arg")
                     if arg.attrib["direction"] == "in")
    outputs = "".join(arg.attrib["type"] for arg in method.findall("arg")
                      if arg.attrib["direction"] == "out")
    return inputs, outputs


def method_arguments(interface: ET.Element, name: str) -> list[str]:
    method = next(node for node in interface.findall("method")
                  if node.attrib["name"] == name)
    return [arg.attrib["name"] for arg in method.findall("arg")]


class LayoutBridgeProtocolTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = ET.parse(PROTOCOL).getroot()
        self.interfaces = {node.attrib["name"]: node
                           for node in self.root.findall("interface")}

    def test_frozen_method_signatures(self) -> None:
        extension = self.interfaces["org.punto.Switcher.InputSource1"]
        peer = self.interfaces["org.punto.Switcher.SessionBridge1"]

        self.assertEqual(method_signature(extension, "BindHelper"),
                         ("sut", "us"))
        self.assertEqual(method_signature(extension, "GetState"),
                         ("sst", "ussttty"))
        self.assertEqual(method_signature(peer, "GetState"),
                         ("st", "usssttty"))
        self.assertEqual(method_signature(peer, "ObserveEditorOrigin"),
                         ("suts", "ustssottuiiiiiss"))
        self.assertEqual(method_signature(peer, "PrepareEditorOrigin"),
                         ("suts", "ustssottuiiiiiss"))
        self.assertEqual(method_signature(peer, "ObserveEditor"),
                         ("sutsiis", "ustssottuiiiiiss"))
        self.assertEqual(method_signature(extension, "Switch"),
                         ("sssststttyy", "usstty"))
        self.assertEqual(method_signature(peer, "Switch"),
                         ("sssststttyy", "usstty"))
        self.assertEqual(method_signature(extension, "GetOperation"),
                         ("sssstst", "usstty"))
        self.assertEqual(method_signature(peer, "GetOperation"),
                         ("sssstst", "usstty"))

    def test_wire_has_only_fixed_scalars(self) -> None:
        allowed = set("sutyio")
        for arg in self.root.findall("./interface/method/arg"):
            self.assertIn(arg.attrib["type"], allowed)
            self.assertIn(arg.attrib["direction"], {"in", "out"})

    def test_desktop_observer_projects_no_private_lease(self) -> None:
        observer = self.interfaces["org.punto.Switcher.DesktopObserver1"]
        self.assertEqual([node.attrib["name"] for node in observer.findall("method")],
                         ["ReadState"])
        self.assertEqual(method_signature(observer, "ReadState"), ("t", "usstty"))
        self.assertEqual(method_arguments(observer, "ReadState"), [
            "deadline_us", "outcome", "helper_instance", "extension_instance",
            "source_generation", "state_generation", "current_role",
        ])
        self.assertNotIn("lease", ET.tostring(observer, encoding="unicode"))

    def test_desktop_observer_does_not_share_private_forward_capacity(self) -> None:
        source = (ROOT / "cpp" / "src" / "session_bridge.cpp").read_text()
        self.assertTrue("struct ObserverCall" in source, "public observer context missing")
        begin = source.index("void SessionBridge::handle_observer(")
        end = source.index("void SessionBridge::handle_method(", begin)
        observer = source[begin:end]
        self.assertNotIn("connections_", observer)
        self.assertNotIn("pending_forwards_", observer)
        self.assertNotIn("call_extension(", observer)
        self.assertNotIn("g_dbus_connection_call_sync", observer)
        self.assertNotIn("authority_current(", observer)
        self.assertIn("GetConnectionUnixUser", observer)
        self.assertIn("NameOwnerChanged", observer)
        self.assertIn('"GetState"', observer)
        self.assertNotIn('"Switch"', observer)
        self.assertNotIn('"GetOperation"', observer)

    def test_desktop_observer_lifetime_and_single_client_are_bounded(self) -> None:
        source = (ROOT / "cpp" / "src" / "session_bridge.cpp").read_text()
        self.assertTrue("kObserverHorizonUs = 100'000" in source, "100ms horizon missing")
        self.assertEqual(source.count("observer_client_id_ = make_id()"), 1)
        self.assertIn("g_source_set_ready_time", source)
        self.assertIn("observer_call_ != nullptr", source)
        self.assertIn("g_cancellable_cancel(observer_call_->cancel)", source)
        self.assertIn("observer_registration_", source)

    def test_descriptor_is_published_after_owned_observer_is_ready(self) -> None:
        source = (ROOT / "cpp" / "src" / "session_bridge.cpp").read_text()
        begin = source.index("bool SessionBridge::start() {")
        end = source.index("void SessionBridge::run()", begin)
        startup = source[begin:end]
        accessible = startup.index("std::make_unique<punto::AccessibleContext>")
        registered = startup.index("observer_registration_ =")
        registration_checked = startup.index("if (observer_registration_ == 0)")
        published = startup.index("if (!publish_descriptor())")
        self.assertLess(accessible, published)
        self.assertLess(registered, published)
        self.assertLess(registration_checked, published)

    def test_gjs_method_evidence_is_published_before_reply(self) -> None:
        fixture = (ROOT / "tests" / "session_bridge_gjs_fixture.mjs").read_text()
        begin = fixture.index("implementation[key] = (parameters, invocation) => {")
        end = fixture.index("};", begin)
        wrapper = fixture[begin:end]
        increment = wrapper.index("counts[method] += 1;")
        published = wrapper.index("saveCounts();")
        reply = wrapper.index("original(parameters, invocation);")
        self.assertLess(increment, published)
        self.assertLess(published, reply)

    def test_desktop_observer_expiry_is_not_authority_drift(self) -> None:
        source = (ROOT / "cpp/src/session_bridge.cpp").read_text()
        begin = source.index("bool SessionBridge::advance_observer(")
        end = source.index("void SessionBridge::handle_method(", begin)
        advance = source[begin:end]
        resolver = advance.index("session_resolver_.revalidate(")
        uid_branch = advance[:resolver].rpartition("} else {")[2]
        self.assertIn("if (now_us() >= call.deadline)", uid_branch)
        self.assertIn("finish_observer(call, kOutcomeDeadlineExpired)", uid_branch)
        self.assertNotIn("&& now_us()", uid_branch)
        after_resolver = advance[resolver:]
        expiry = after_resolver.index("if (now_us() >= call.deadline)")
        drift = after_resolver.index("if (!current)")
        self.assertLess(expiry, drift)
        self.assertNotIn("request_stop", after_resolver[expiry:drift])
        self.assertIn("request_stop(true)", after_resolver[drift:])

    def test_frozen_argument_names(self) -> None:
        extension = self.interfaces["org.punto.Switcher.InputSource1"]
        peer = self.interfaces["org.punto.Switcher.SessionBridge1"]
        common_switch = [
            "helper_instance", "extension_instance", "client_instance",
            "client_lease_token", "sequence", "request_digest",
            "original_deadline_us", "expected_source_generation",
            "expected_state_generation", "expected_role", "target_role",
            "outcome", "result_extension_instance", "result_request_digest",
            "result_source_generation", "result_state_generation", "result_role",
        ]
        common_operation = [
            "helper_instance", "extension_instance", "client_instance",
            "client_lease_token", "sequence", "request_digest",
            "query_deadline_us", "outcome", "result_extension_instance",
            "result_request_digest", "result_source_generation",
            "result_state_generation", "result_role",
        ]
        self.assertEqual(method_arguments(extension, "BindHelper"), [
            "helper_instance", "protocol_version", "deadline_us", "outcome",
            "extension_instance",
        ])
        self.assertEqual(method_arguments(extension, "GetState"), [
            "helper_instance", "client_instance", "deadline_us", "outcome",
            "extension_instance", "client_lease_token", "client_lease_expiry_us",
            "source_generation", "state_generation", "current_role",
        ])
        self.assertEqual(method_arguments(peer, "GetState"), [
            "client_instance", "deadline_us", "outcome", "helper_instance",
            "extension_instance", "client_lease_token", "client_lease_expiry_us",
            "source_generation", "state_generation", "current_role",
        ])
        self.assertEqual(method_arguments(extension, "Switch"), common_switch)
        self.assertEqual(method_arguments(peer, "Switch"), common_switch)
        self.assertEqual(method_arguments(extension, "GetOperation"),
                         common_operation)
        self.assertEqual(method_arguments(peer, "GetOperation"), common_operation)

    def test_metadata_and_extension_copy_are_frozen(self) -> None:
        annotations = {node.attrib["name"]: node.attrib["value"]
                       for node in self.root.findall("annotation")}
        self.assertEqual(annotations, {
            "org.punto.Switcher.ProtocolVersion": "1",
            "org.punto.Switcher.IdHexLength": "32",
            "org.punto.Switcher.DigestHexLength": "64",
            "org.punto.Switcher.MaxConnections": "4",
            "org.punto.Switcher.MaxMessageBytes": "1024",
            "org.punto.Switcher.MaxClients": "32",
            "org.punto.Switcher.MaxOperations": "128",
            "org.punto.Switcher.SwitchHorizonUs": "1000000",
            "org.punto.Switcher.ClientLeaseUs": "30000000",
            "org.punto.Switcher.ReconnectGraceUs": "2500000",
            "org.punto.Switcher.Outcomes": (
                "State=0,GnomeStateApplied=1,AlreadyCurrent=2,"
                "UnsupportedState=10,StaleState=11,Conflict=12,Expired=13,"
                "Capacity=14,Locked=15,Diverged=16,Failed=17,NotFound=18,"
                "VersionMismatch=19,HelperStale=20,DeadlineExpired=21,"
                "HopUnknown=22"),
            "org.punto.Switcher.Roles": "EN=0,RU=1,Unknown=255",
            "org.punto.Switcher.DigestFields": (
                "version,extension_instance,expected_source_generation,"
                "expected_state_generation,expected_role,target_role"),
        })
        self.assertEqual(EXTENSION_PROTOCOL.read_bytes(), PROTOCOL.read_bytes())

    def test_installed_gnome46_adapter_surface(self) -> None:
        expected = {
            "schema": 1,
            "shell_major": 46,
            "library_sha256": (
                "35f439ff850147ccf01fae2d715f6925"
                "2eb9f8631cb5702e9449c88563cb8885"),
            "reviewed_resource_sha256": {
                "/org/gnome/shell/ui/status/keyboard.js": [
                    "adf1af6d9d7fb888c429cc8e2fa7339b2ea5a23806fe1439c9934edbb9ff7285"
                ],
            },
            "resources": {
                "/org/gnome/shell/ui/status/keyboard.js": (
                    "ede25eee29bf2b3520ef535e76913229"
                    "75596653a479582ece7ff33993e139c1"),
                "/org/gnome/shell/misc/ibusManager.js": (
                    "1b15579d257a3811cebba48ccb7aa85"
                    "381b10f74780487ce5ff1636354b274c4"),
                "/org/gnome/shell/misc/keyboardManager.js": (
                    "d292f164095249942c454674c7e19bfd"
                    "b007fea3d39e65be25714c4c3fe4ae2a"),
            },
        }
        contract = json.loads(GNOME_CONTRACT.read_text(encoding="ascii"))
        self.assertEqual(contract, expected)
        library = Path("/usr/lib/gnome-shell/libshell-14.so")
        if not library.exists():
            self.skipTest("GNOME Shell 46 resource library is not installed")

        # The ELF digest records the original package, not the adapter API.
        # Only explicitly reviewed resource bytes may satisfy this contract.

        def resource(path: str) -> str:
            result = subprocess.run(
                ["gresource", "extract", str(library), path],
                check=True, capture_output=True, text=True)
            return result.stdout

        extracted = {path: resource(path)
                     for path in contract["resources"]}
        for path, digest in contract["resources"].items():
            allowed = [digest, *contract["reviewed_resource_sha256"].get(path, [])]
            self.assertIn(hashlib.sha256(
                extracted[path].encode()).hexdigest(), allowed)
        keyboard = extracted["/org/gnome/shell/ui/status/keyboard.js"]
        manager = extracted["/org/gnome/shell/misc/keyboardManager.js"]
        for fragment in (
            "get inputSources()", "get currentSource()",
            "get keyboardManager()", "this.emit('sources-changed')",
            "this.emit('current-source-changed'", "this.xkbId =",
            "activate(interactive)",
            "this._keyboardManager.apply(is.xkbId)",
            "this._ibusManager.setEngine(engine, KeyboardManager.releaseKeyboard)",
            "this._currentInputSourceChanged(is, interactive)",
        ):
            self.assertIn(fragment, keyboard)
        self.assertIn("get currentLayout()", manager)
        self.assertIn("this._current = info", manager)


if __name__ == "__main__":
    unittest.main()
