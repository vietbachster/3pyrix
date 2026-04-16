#include "HyphenationCommon.h"

#include <Utf8.h>

#include <algorithm>

namespace {

uint32_t toLowerLatinImpl(const uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') {
    return cp - 'A' + 'a';
  }
  if ((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00DE)) {
    return cp + 0x20;
  }
  // Latin Extended Additional (U+1E00-U+1EFF): uppercase at even, lowercase at odd
  if (cp >= 0x1E00 && cp <= 0x1EFF && (cp & 1) == 0 && cp != 0x1E9E) {
    return cp + 1;
  }

  switch (cp) {
    case 0x0102: return 0x0103;  // Ă → ă
    case 0x0110: return 0x0111;  // Đ → đ
    case 0x01A0: return 0x01A1;  // Ơ → ơ
    case 0x01AF: return 0x01B0;  // Ư → ư
    case 0x0152:      // Œ
      return 0x0153;  // œ
    case 0x0178:      // Ÿ
      return 0x00FF;  // ÿ
    case 0x1E9E:      // ẞ
      return 0x00DF;  // ß
    default:
      return cp;
  }
}

uint32_t toLowerCyrillicImpl(const uint32_t cp) {
  if (cp >= 0x0410 && cp <= 0x042F) {
    return cp + 0x20;
  }
  if (cp == 0x0401) {
    return 0x0451;
  }
  return cp;
}

}  // namespace

uint32_t toLowerLatin(const uint32_t cp) { return toLowerLatinImpl(cp); }

uint32_t toLowerCyrillic(const uint32_t cp) { return toLowerCyrillicImpl(cp); }

bool isLatinLetter(const uint32_t cp) {
  if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
    return true;
  }

  if (((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00F6) || (cp >= 0x00F8 && cp <= 0x00FF)) &&
      cp != 0x00D7 && cp != 0x00F7) {
    return true;
  }

  // Latin Extended-A/B (U+0100-U+024F): đ (U+0111), ơ (U+01A1), ư (U+01B0), ă (U+0103)...
  if (cp >= 0x0100 && cp <= 0x024F) {
    return true;
  }

  // Latin Extended Additional (U+1E00-U+1EFF): toàn bộ ký tự có dấu tiếng Việt precomposed
  // e.g. ắ (U+1EAF), ặ (U+1EB7), ầ (U+1EA7), ề (U+1EC1), ổ (U+1ED5), ữ (U+1EEF)...
  if (cp >= 0x1E00 && cp <= 0x1EFF) {
    return true;
  }

  switch (cp) {
    case 0x0152:  // Œ
    case 0x0153:  // œ
    case 0x0178:  // Ÿ
    case 0x1E9E:  // ẞ
      return true;
    default:
      return false;
  }
}

bool isCyrillicLetter(const uint32_t cp) { return (cp >= 0x0400 && cp <= 0x052F); }

bool isAlphabetic(const uint32_t cp) { return isLatinLetter(cp) || isCyrillicLetter(cp); }

bool isPunctuation(const uint32_t cp) {
  switch (cp) {
    case '-':
    case '.':
    case ',':
    case '!':
    case '?':
    case ';':
    case ':':
    case '"':
    case '\'':
    case ')':
    case '(':
    case 0x00AB:  // «
    case 0x00BB:  // »
    case 0x2018:  // '
    case 0x2019:  // '
    case 0x201C:  // "
    case 0x201D:  // "
    case 0x00A0:  // no-break space
    case '{':
    case '}':
    case '[':
    case ']':
    case '/':
    case 0x203A:  // ›
    case 0x2026:  // …
      return true;
    default:
      return false;
  }
}

bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

bool isExplicitHyphen(const uint32_t cp) {
  switch (cp) {
    case '-':
    case 0x00AD:  // soft hyphen
    case 0x058A:  // Armenian hyphen
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x2012:  // figure dash
    case 0x2013:  // en dash
    case 0x2014:  // em dash
    case 0x2015:  // horizontal bar
    case 0x2043:  // hyphen bullet
    case 0x207B:  // superscript minus
    case 0x208B:  // subscript minus
    case 0x2212:  // minus sign
    case 0x2E17:  // double oblique hyphen
    case 0x2E3A:  // two-em dash
    case 0x2E3B:  // three-em dash
    case 0xFE58:  // small em dash
    case 0xFE63:  // small hyphen-minus
    case 0xFF0D:  // fullwidth hyphen-minus
    case 0x005F:  // Underscore
    case 0x2026:  // Ellipsis
      return true;
    default:
      return false;
  }
}

bool isSoftHyphen(const uint32_t cp) { return cp == 0x00AD; }

void trimSurroundingPunctuationAndFootnote(std::vector<CodepointInfo>& cps) {
  if (cps.empty()) {
    return;
  }

  // Remove trailing footnote references like [12], even if punctuation trails after the closing bracket.
  if (cps.size() >= 3) {
    int end = static_cast<int>(cps.size()) - 1;
    while (end >= 0 && isPunctuation(cps[end].value)) {
      --end;
    }
    int pos = end;
    if (pos >= 0 && isAsciiDigit(cps[pos].value)) {
      while (pos >= 0 && isAsciiDigit(cps[pos].value)) {
        --pos;
      }
      if (pos >= 0 && cps[pos].value == '[' && end - pos > 1) {
        cps.erase(cps.begin() + pos, cps.end());
      }
    }
  }

  {
    auto first = std::find_if(cps.begin(), cps.end(), [](const CodepointInfo& c) { return !isPunctuation(c.value); });
    cps.erase(cps.begin(), first);
  }
  while (!cps.empty() && isPunctuation(cps.back().value)) {
    cps.pop_back();
  }
}

namespace {

uint32_t composeNfc(const uint32_t base, const uint32_t combining) {
  // clang-format off
  switch (combining) {
    case 0x0300:  // combining grave
      switch (base) {
        case 'A': return 0x00C0; case 'E': return 0x00C8; case 'I': return 0x00CC;
        case 'O': return 0x00D2; case 'U': return 0x00D9;
        case 'a': return 0x00E0; case 'e': return 0x00E8; case 'i': return 0x00EC;
        case 'o': return 0x00F2; case 'u': return 0x00F9;
        // Vietnamese: base + circumflex/breve/horn + grave
        case 0x00C2: return 0x1EA6; case 0x00E2: return 0x1EA7;  // Â/â → Ầ/ầ
        case 0x00CA: return 0x1EC0; case 0x00EA: return 0x1EC1;  // Ê/ê → Ề/ề
        case 0x00D4: return 0x1ED2; case 0x00F4: return 0x1ED3;  // Ô/ô → Ồ/ồ
        case 0x0102: return 0x1EB0; case 0x0103: return 0x1EB1;  // Ă/ă → Ằ/ằ
        case 0x01A0: return 0x1EDC; case 0x01A1: return 0x1EDD;  // Ơ/ơ → Ờ/ờ
        case 0x01AF: return 0x1EEA; case 0x01B0: return 0x1EEB;  // Ư/ư → Ừ/ừ
        default: return 0;
      }
    case 0x0301:  // combining acute
      switch (base) {
        case 'A': return 0x00C1; case 'E': return 0x00C9; case 'I': return 0x00CD;
        case 'O': return 0x00D3; case 'U': return 0x00DA; case 'Y': return 0x00DD;
        case 'a': return 0x00E1; case 'e': return 0x00E9; case 'i': return 0x00ED;
        case 'o': return 0x00F3; case 'u': return 0x00FA; case 'y': return 0x00FD;
        // Vietnamese: base + circumflex/breve/horn + acute
        case 0x00C2: return 0x1EA4; case 0x00E2: return 0x1EA5;  // Â/â → Ấ/ấ
        case 0x00CA: return 0x1EBE; case 0x00EA: return 0x1EBF;  // Ê/ê → Ế/ế
        case 0x00D4: return 0x1ED0; case 0x00F4: return 0x1ED1;  // Ô/ô → Ố/ố
        case 0x0102: return 0x1EAE; case 0x0103: return 0x1EAF;  // Ă/ă → Ắ/ắ
        case 0x01A0: return 0x1EDA; case 0x01A1: return 0x1EDB;  // Ơ/ơ → Ớ/ớ
        case 0x01AF: return 0x1EE8; case 0x01B0: return 0x1EE9;  // Ư/ư → Ứ/ứ
        default: return 0;
      }
    case 0x0302:  // combining circumflex
      switch (base) {
        case 'A': return 0x00C2; case 'E': return 0x00CA; case 'I': return 0x00CE;
        case 'O': return 0x00D4; case 'U': return 0x00DB;
        case 'a': return 0x00E2; case 'e': return 0x00EA; case 'i': return 0x00EE;
        case 'o': return 0x00F4; case 'u': return 0x00FB;
        default: return 0;
      }
    case 0x0303:  // combining tilde
      switch (base) {
        case 'A': return 0x00C3; case 'N': return 0x00D1; case 'O': return 0x00D5;
        case 'a': return 0x00E3; case 'n': return 0x00F1; case 'o': return 0x00F5;
        // Vietnamese: base + circumflex/breve/horn + tilde
        case 0x00C2: return 0x1EAA; case 0x00E2: return 0x1EAB;  // Â/â → Ẫ/ẫ
        case 0x00CA: return 0x1EC4; case 0x00EA: return 0x1EC5;  // Ê/ê → Ễ/ễ
        case 0x00D4: return 0x1ED6; case 0x00F4: return 0x1ED7;  // Ô/ô → Ỗ/ỗ
        case 0x0102: return 0x1EB4; case 0x0103: return 0x1EB5;  // Ă/ă → Ẵ/ẵ
        case 0x01A0: return 0x1EE0; case 0x01A1: return 0x1EE1;  // Ơ/ơ → Ỡ/ỡ
        case 0x01AF: return 0x1EEE; case 0x01B0: return 0x1EEF;  // Ư/ư → Ữ/ữ
        default: return 0;
      }
    case 0x0306:  // combining breve (Vietnamese: ă)
      switch (base) {
        case 'A': return 0x0102; case 'a': return 0x0103;  // A/a → Ă/ă
        default: return 0;
      }
    case 0x0308:  // combining diaeresis
      switch (base) {
        case 'A': return 0x00C4; case 'E': return 0x00CB; case 'I': return 0x00CF;
        case 'O': return 0x00D6; case 'U': return 0x00DC;
        case 'a': return 0x00E4; case 'e': return 0x00EB; case 'i': return 0x00EF;
        case 'o': return 0x00F6; case 'u': return 0x00FC; case 'y': return 0x00FF;
        default: return 0;
      }
    case 0x0309:  // combining hook above (dấu hỏi)
      switch (base) {
        case 'A': return 0x1EA2; case 'a': return 0x1EA3;
        case 'E': return 0x1EBA; case 'e': return 0x1EBB;
        case 'I': return 0x1EC8; case 'i': return 0x1EC9;
        case 'O': return 0x1ECE; case 'o': return 0x1ECF;
        case 'U': return 0x1EE6; case 'u': return 0x1EE7;
        case 'Y': return 0x1EF6; case 'y': return 0x1EF7;
        case 0x00C2: return 0x1EA8; case 0x00E2: return 0x1EA9;  // Â/â → Ẩ/ẩ
        case 0x00CA: return 0x1EC2; case 0x00EA: return 0x1EC3;  // Ê/ê → Ể/ể
        case 0x00D4: return 0x1ED4; case 0x00F4: return 0x1ED5;  // Ô/ô → Ổ/ổ
        case 0x0102: return 0x1EB2; case 0x0103: return 0x1EB3;  // Ă/ă → Ẳ/ẳ
        case 0x01A0: return 0x1EDE; case 0x01A1: return 0x1EDF;  // Ơ/ơ → Ở/ở
        case 0x01AF: return 0x1EEC; case 0x01B0: return 0x1EED;  // Ư/ư → Ử/ử
        default: return 0;
      }
    case 0x031B:  // combining horn (Vietnamese: ơ, ư)
      switch (base) {
        case 'O': return 0x01A0; case 'o': return 0x01A1;  // O/o → Ơ/ơ
        case 'U': return 0x01AF; case 'u': return 0x01B0;  // U/u → Ư/ư
        default: return 0;
      }
    case 0x0323:  // combining dot below (dấu nặng)
      switch (base) {
        case 'A': return 0x1EA0; case 'a': return 0x1EA1;
        case 'E': return 0x1EB8; case 'e': return 0x1EB9;
        case 'I': return 0x1ECA; case 'i': return 0x1ECB;
        case 'O': return 0x1ECC; case 'o': return 0x1ECD;
        case 'U': return 0x1EE4; case 'u': return 0x1EE5;
        case 'Y': return 0x1EF4; case 'y': return 0x1EF5;
        case 0x00C2: return 0x1EAC; case 0x00E2: return 0x1EAD;  // Â/â → Ậ/ậ
        case 0x00CA: return 0x1EC6; case 0x00EA: return 0x1EC7;  // Ê/ê → Ệ/ệ
        case 0x00D4: return 0x1ED8; case 0x00F4: return 0x1ED9;  // Ô/ô → Ộ/ộ
        case 0x0102: return 0x1EB6; case 0x0103: return 0x1EB7;  // Ă/ă → Ặ/ặ
        case 0x01A0: return 0x1EE2; case 0x01A1: return 0x1EE3;  // Ơ/ơ → Ợ/ợ
        case 0x01AF: return 0x1EF0; case 0x01B0: return 0x1EF1;  // Ư/ư → Ự/ự
        default: return 0;
      }
    case 0x0327:  // combining cedilla
      switch (base) {
        case 'C': return 0x00C7; case 'c': return 0x00E7;
        default: return 0;
      }
    default:
      return 0;
  }
  // clang-format on
}

}  // namespace

std::vector<CodepointInfo> collectCodepoints(const std::string& word) {
  std::vector<CodepointInfo> cps;
  cps.reserve(word.size());

  const unsigned char* base = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* ptr = base;
  while (*ptr != 0) {
    const unsigned char* current = ptr;
    const uint32_t cp = utf8NextCodepoint(&ptr);

    if (!cps.empty() && cp >= 0x0300 && cp <= 0x036F) {
      const uint32_t composed = composeNfc(cps.back().value, cp);
      if (composed != 0) {
        cps.back().value = composed;
        continue;
      }
    }

    cps.push_back({cp, static_cast<size_t>(current - base)});
  }

  return cps;
}
