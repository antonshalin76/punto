from __future__ import annotations

class LanguageDetector:
    def __init__(self) -> None:
        self.en_vowels = set("aeiouy")
        self.ru_vowels = set("аеёиоуыэюя")

    def analyze(self, en_text: str, ru_text: str) -> str | None:
        """
        Returns 'en' if English is more likely, 'ru' if Russian, or None if unsure.
        """
        score_en = self._score_structure(en_text, "en")
        score_ru = self._score_structure(ru_text, "ru")
        
        # Simple threshold
        if score_en > 0 and score_ru < 0:
            return "en"
        if score_ru > 0 and score_en < 0:
            return "ru"
            
        # If both valid, check specific impossible trigrams (TODO)
        return None

    def _score_structure(self, text: str, lang: str) -> int:
        """
        Returns positive for likely valid, negative for likely invalid.
        """
        if not text:
            return 0
            
        vowels = self.en_vowels if lang == "en" else self.ru_vowels
        
        consecutive_consonants = 0
        max_consecutive_consonants = 0
        
        for char in text.lower():
            if not char.isalpha():
                continue # punctuation resets? or ignore?
            
            if char in vowels:
                consecutive_consonants = 0
            else:
                consecutive_consonants += 1
                max_consecutive_consonants = max(max_consecutive_consonants, consecutive_consonants)

        # Penalize huge consonant clusters
        # EN: "strength" (str, ngth) -> up to 5 is theoretically possible but rare.
        # RU: "взгляд" (vzgl) -> 4. "контрстратегия" (ntrstr)
        
        limit = 5 if lang == "en" else 4
        
        if max_consecutive_consonants > limit:
            return -10
            
        # Penalize NO vowels in long word
        if len(text) > 4 and not any(c in vowels for c in text.lower()):
            return -5

        return 5
