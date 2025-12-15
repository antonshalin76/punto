from __future__ import annotations

import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .errors import ConfigurationError

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class Config:
    hotkeys: dict[str, list[int]]
    sound_enabled: bool
    auto_switch_enabled: bool
    exceptions: dict[str, list[str]]
    autocorrect: dict[str, str]
    autoreplace: dict[str, str]
    layout_switch_hotkey: list[int]


class ConfigManager:
    def __init__(self, config_dir: Path | None = None) -> None:
        self._config_dir = config_dir if config_dir is not None else Path.home() / ".config" / "punto-switcher"

    @property
    def config_dir(self) -> Path:
        return self._config_dir

    def get_default(self) -> Config:
        return Config(
            hotkeys={
                "convert_word": [119],
                "convert_phrase": [42, 119],
                "convert_selection": [29, 42, 119],
                "transliterate": [29, 42, 25],
            },
            sound_enabled=True,
            auto_switch_enabled=True,
            exceptions={"processes": [], "window_titles": []},
            autocorrect={},
            autoreplace={},
            layout_switch_hotkey=[125, 57],
        )

    def load(self) -> Config:
        self._config_dir.mkdir(parents=True, exist_ok=True)

        config_path = self._config_dir / "config.json"
        autocorrect_path = self._config_dir / "autocorrect.json"
        autoreplace_path = self._config_dir / "autoreplace.json"
        exceptions_path = self._config_dir / "exceptions.json"

        if not config_path.exists():
            config = self.get_default()
            self.save(config)
            return config

        if not autocorrect_path.exists():
            raise ConfigurationError(f"Отсутствует файл конфигурации: {autocorrect_path}")
        if not autoreplace_path.exists():
            raise ConfigurationError(f"Отсутствует файл конфигурации: {autoreplace_path}")
        if not exceptions_path.exists():
            raise ConfigurationError(f"Отсутствует файл конфигурации: {exceptions_path}")

        base = self._read_json_dict(config_path)
        autocorrect = self._read_json_dict(autocorrect_path)
        autoreplace = self._read_json_dict(autoreplace_path)
        exceptions = self._read_json_dict(exceptions_path)

        return Config(
            hotkeys=self._require_hotkeys(base, field_name="hotkeys", path=config_path),
            sound_enabled=self._require_bool(base, field_name="sound_enabled", path=config_path),
            auto_switch_enabled=self._require_bool(base, field_name="auto_switch_enabled", path=config_path),
            layout_switch_hotkey=self._require_int_list(base, field_name="layout_switch_hotkey", path=config_path),
            exceptions=self._require_exceptions(exceptions, path=exceptions_path),
            autocorrect=self._require_str_dict(autocorrect, path=autocorrect_path),
            autoreplace=self._require_str_dict(autoreplace, path=autoreplace_path),
        )

    def save(self, config: Config) -> None:
        self._config_dir.mkdir(parents=True, exist_ok=True)

        self._write_json(
            path=self._config_dir / "config.json",
            payload={
                "auto_switch_enabled": config.auto_switch_enabled,
                "sound_enabled": config.sound_enabled,
                "hotkeys": config.hotkeys,
                "layout_switch_hotkey": config.layout_switch_hotkey,
            },
        )
        self._write_json(path=self._config_dir / "autocorrect.json", payload=config.autocorrect)
        self._write_json(path=self._config_dir / "autoreplace.json", payload=config.autoreplace)
        self._write_json(path=self._config_dir / "exceptions.json", payload=config.exceptions)

    def _read_json_dict(self, path: Path) -> dict[str, Any]:
        try:
            raw = path.read_text(encoding="utf-8")
        except OSError as exc:
            logger.exception("Failed to read JSON file")
            raise ConfigurationError(f"Не удалось прочитать файл: {path}") from exc

        try:
            data = json.loads(raw)
        except json.JSONDecodeError as exc:
            logger.exception("Failed to parse JSON")
            raise ConfigurationError(f"Некорректный JSON в файле: {path}") from exc

        if not isinstance(data, dict):
            raise ConfigurationError(f"Ожидался JSON-объект в файле: {path}")

        return data

    def _write_json(self, path: Path, payload: Any) -> None:
        try:
            encoded = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True)
            path.write_text(encoded + "\n", encoding="utf-8")
        except OSError as exc:
            logger.exception("Failed to write JSON")
            raise ConfigurationError(f"Не удалось записать файл: {path}") from exc

    def _require_bool(self, source: dict[str, Any], field_name: str, path: Path) -> bool:
        value = source.get(field_name)
        if isinstance(value, bool):
            return value
        raise ConfigurationError(f"Поле {field_name} должно быть bool в файле: {path}")

    def _require_int_list(self, source: dict[str, Any], field_name: str, path: Path) -> list[int]:
        value = source.get(field_name)
        if not isinstance(value, list) or not all(isinstance(item, int) for item in value):
            raise ConfigurationError(f"Поле {field_name} должно быть list[int] в файле: {path}")
        return list(value)

    def _require_hotkeys(self, source: dict[str, Any], field_name: str, path: Path) -> dict[str, list[int]]:
        value = source.get(field_name)
        if not isinstance(value, dict):
            raise ConfigurationError(f"Поле {field_name} должно быть объектом в файле: {path}")

        result: dict[str, list[int]] = {}
        for key, raw_list in value.items():
            if not isinstance(key, str):
                raise ConfigurationError(f"Ключи {field_name} должны быть строками в файле: {path}")
            if not isinstance(raw_list, list) or not all(isinstance(item, int) for item in raw_list):
                raise ConfigurationError(f"Значения {field_name}.{key} должны быть list[int] в файле: {path}")
            result[key] = list(raw_list)
        return result

    def _require_str_dict(self, source: dict[str, Any], path: Path) -> dict[str, str]:
        result: dict[str, str] = {}
        for key, value in source.items():
            if not isinstance(key, str) or not isinstance(value, str):
                raise ConfigurationError(f"Ожидался словарь str->str в файле: {path}")
            result[key] = value
        return result

    def _require_exceptions(self, source: dict[str, Any], path: Path) -> dict[str, list[str]]:
        processes = source.get("processes")
        window_titles = source.get("window_titles")
        if not isinstance(processes, list) or not all(isinstance(item, str) for item in processes):
            raise ConfigurationError(f"Поле processes должно быть list[str] в файле: {path}")
        if not isinstance(window_titles, list) or not all(isinstance(item, str) for item in window_titles):
            raise ConfigurationError(f"Поле window_titles должно быть list[str] в файле: {path}")
        return {"processes": list(processes), "window_titles": list(window_titles)}
