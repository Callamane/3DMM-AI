/***************************************************************************
    Automation bridge: JSON parser and serializer.
***************************************************************************/
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace automation
{

static const Json kjsonNull;

// Nesting limit so a hostile request can't exhaust the stack
const int kcNestMax = 64;

const Json &Json::Get(const std::string &key) const
{
    if (_type != Type::Object)
        return kjsonNull;
    for (const Member &member : _object)
    {
        if (member.first == key)
            return member.second;
    }
    return kjsonNull;
}

bool Json::Has(const std::string &key) const
{
    if (_type != Type::Object)
        return false;
    for (const Member &member : _object)
    {
        if (member.first == key)
            return true;
    }
    return false;
}

Json &Json::Set(const std::string &key, Json value)
{
    if (_type == Type::Null)
        _type = Type::Object;
    for (Member &member : _object)
    {
        if (member.first == key)
        {
            member.second = std::move(value);
            return member.second;
        }
    }
    _object.emplace_back(key, std::move(value));
    return _object.back().second;
}

void Json::Push(Json value)
{
    if (_type == Type::Null)
        _type = Type::Array;
    _array.push_back(std::move(value));
}

size_t Json::Size() const
{
    if (_type == Type::Array)
        return _array.size();
    if (_type == Type::Object)
        return _object.size();
    return 0;
}

void JsonQuote(const std::string &str, std::string *pstrOut)
{
    pstrOut->push_back('"');
    for (unsigned char ch : str)
    {
        switch (ch)
        {
        case '"':
            pstrOut->append("\\\"");
            break;
        case '\\':
            pstrOut->append("\\\\");
            break;
        case '\n':
            pstrOut->append("\\n");
            break;
        case '\r':
            pstrOut->append("\\r");
            break;
        case '\t':
            pstrOut->append("\\t");
            break;
        case '\b':
            pstrOut->append("\\b");
            break;
        case '\f':
            pstrOut->append("\\f");
            break;
        default:
            if (ch < 0x20)
            {
                char rgch[8];
                snprintf(rgch, sizeof(rgch), "\\u%04x", ch);
                pstrOut->append(rgch);
            }
            else
            {
                pstrOut->push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    pstrOut->push_back('"');
}

static void _DumpNumber(double d, std::string *pstr)
{
    char rgch[32];

    if (!std::isfinite(d))
    {
        pstr->append("null");
        return;
    }
    if (d == std::floor(d) && std::fabs(d) < 9007199254740992.0)
        snprintf(rgch, sizeof(rgch), "%.0f", d);
    else
        snprintf(rgch, sizeof(rgch), "%.6g", d);
    pstr->append(rgch);
}

void Json::DumpTo(std::string *pstr) const
{
    switch (_type)
    {
    case Type::Null:
        pstr->append("null");
        break;
    case Type::Bool:
        pstr->append(_bool ? "true" : "false");
        break;
    case Type::Number:
        _DumpNumber(_number, pstr);
        break;
    case Type::String:
        JsonQuote(_string, pstr);
        break;
    case Type::Array: {
        pstr->push_back('[');
        bool fFirst = true;
        for (const Json &json : _array)
        {
            if (!fFirst)
                pstr->push_back(',');
            fFirst = false;
            json.DumpTo(pstr);
        }
        pstr->push_back(']');
        break;
    }
    case Type::Object: {
        pstr->push_back('{');
        bool fFirst = true;
        for (const Member &member : _object)
        {
            if (!fFirst)
                pstr->push_back(',');
            fFirst = false;
            JsonQuote(member.first, pstr);
            pstr->push_back(':');
            member.second.DumpTo(pstr);
        }
        pstr->push_back('}');
        break;
    }
    }
}

std::string Json::Dump() const
{
    std::string str;
    DumpTo(&str);
    return str;
}

/***************************************************************************
    Recursive-descent parser
***************************************************************************/
class JsonParser
{
  public:
    JsonParser(const std::string &str) : _str(str)
    {
    }

    bool FParseDocument(Json *pjson, std::string *pstrError)
    {
        _SkipSpace();
        if (!_FParseValue(pjson, 0))
        {
            *pstrError = _strError;
            return false;
        }
        _SkipSpace();
        if (_ich != _str.size())
        {
            _Fail("unexpected trailing characters");
            *pstrError = _strError;
            return false;
        }
        return true;
    }

  private:
    const std::string &_str;
    size_t _ich = 0;
    std::string _strError;

    bool _Fail(const char *pszMessage)
    {
        if (_strError.empty())
            _strError = std::string(pszMessage) + " at offset " + std::to_string(_ich);
        return false;
    }

    void _SkipSpace()
    {
        while (_ich < _str.size() &&
               (_str[_ich] == ' ' || _str[_ich] == '\t' || _str[_ich] == '\n' || _str[_ich] == '\r'))
        {
            _ich++;
        }
    }

    bool _FLiteral(const char *psz)
    {
        size_t cch = std::char_traits<char>::length(psz);
        if (_str.compare(_ich, cch, psz) != 0)
            return _Fail("invalid literal");
        _ich += cch;
        return true;
    }

    bool _FParseValue(Json *pjson, int cNest)
    {
        if (cNest > kcNestMax)
            return _Fail("nesting too deep");
        if (_ich >= _str.size())
            return _Fail("unexpected end of input");

        char ch = _str[_ich];
        switch (ch)
        {
        case '{':
            return _FParseObject(pjson, cNest);
        case '[':
            return _FParseArray(pjson, cNest);
        case '"': {
            std::string str;
            if (!_FParseString(&str))
                return false;
            *pjson = Json(std::move(str));
            return true;
        }
        case 't':
            *pjson = Json(true);
            return _FLiteral("true");
        case 'f':
            *pjson = Json(false);
            return _FLiteral("false");
        case 'n':
            *pjson = Json();
            return _FLiteral("null");
        default:
            if (ch == '-' || (ch >= '0' && ch <= '9'))
                return _FParseNumber(pjson);
            return _Fail("unexpected character");
        }
    }

    bool _FParseNumber(Json *pjson)
    {
        size_t ichStart = _ich;
        if (_str[_ich] == '-')
            _ich++;
        if (_ich >= _str.size() || !(_str[_ich] >= '0' && _str[_ich] <= '9'))
            return _Fail("invalid number");
        while (_ich < _str.size() && ((_str[_ich] >= '0' && _str[_ich] <= '9') || _str[_ich] == '.' ||
                                      _str[_ich] == 'e' || _str[_ich] == 'E' || _str[_ich] == '+' ||
                                      _str[_ich] == '-'))
        {
            _ich++;
        }
        std::string strNum = _str.substr(ichStart, _ich - ichStart);
        char *pchEnd = nullptr;
        double d = strtod(strNum.c_str(), &pchEnd);
        if (pchEnd == nullptr || *pchEnd != '\0')
        {
            _ich = ichStart;
            return _Fail("invalid number");
        }
        *pjson = Json(d);
        return true;
    }

    static void _AppendUtf8(uint32_t cp, std::string *pstr)
    {
        if (cp < 0x80)
        {
            pstr->push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            pstr->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            pstr->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            pstr->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            pstr->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            pstr->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            pstr->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            pstr->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            pstr->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            pstr->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool _FParseHex4(uint32_t *pcp)
    {
        if (_ich + 4 > _str.size())
            return _Fail("truncated \\u escape");
        uint32_t cp = 0;
        for (int i = 0; i < 4; i++)
        {
            char ch = _str[_ich++];
            cp <<= 4;
            if (ch >= '0' && ch <= '9')
                cp |= ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                cp |= ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F')
                cp |= ch - 'A' + 10;
            else
                return _Fail("invalid \\u escape");
        }
        *pcp = cp;
        return true;
    }

    bool _FParseString(std::string *pstr)
    {
        _ich++; // opening quote
        while (true)
        {
            if (_ich >= _str.size())
                return _Fail("unterminated string");
            char ch = _str[_ich++];
            if (ch == '"')
                return true;
            if (static_cast<unsigned char>(ch) < 0x20)
                return _Fail("control character in string");
            if (ch != '\\')
            {
                pstr->push_back(ch);
                continue;
            }
            if (_ich >= _str.size())
                return _Fail("unterminated escape");
            ch = _str[_ich++];
            switch (ch)
            {
            case '"':
            case '\\':
            case '/':
                pstr->push_back(ch);
                break;
            case 'b':
                pstr->push_back('\b');
                break;
            case 'f':
                pstr->push_back('\f');
                break;
            case 'n':
                pstr->push_back('\n');
                break;
            case 'r':
                pstr->push_back('\r');
                break;
            case 't':
                pstr->push_back('\t');
                break;
            case 'u': {
                uint32_t cp;
                if (!_FParseHex4(&cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    uint32_t cpLow;
                    if (_ich + 2 > _str.size() || _str[_ich] != '\\' || _str[_ich + 1] != 'u')
                        return _Fail("unpaired surrogate");
                    _ich += 2;
                    if (!_FParseHex4(&cpLow))
                        return false;
                    if (cpLow < 0xDC00 || cpLow > 0xDFFF)
                        return _Fail("invalid surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (cpLow - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                {
                    return _Fail("unpaired surrogate");
                }
                _AppendUtf8(cp, pstr);
                break;
            }
            default:
                return _Fail("invalid escape");
            }
        }
    }

    bool _FParseArray(Json *pjson, int cNest)
    {
        Json json = Json::MakeArray();
        _ich++; // [
        _SkipSpace();
        if (_ich < _str.size() && _str[_ich] == ']')
        {
            _ich++;
            *pjson = std::move(json);
            return true;
        }
        while (true)
        {
            Json jsonItem;
            _SkipSpace();
            if (!_FParseValue(&jsonItem, cNest + 1))
                return false;
            json.Push(std::move(jsonItem));
            _SkipSpace();
            if (_ich >= _str.size())
                return _Fail("unterminated array");
            char ch = _str[_ich++];
            if (ch == ']')
                break;
            if (ch != ',')
                return _Fail("expected ',' or ']'");
        }
        *pjson = std::move(json);
        return true;
    }

    bool _FParseObject(Json *pjson, int cNest)
    {
        Json json = Json::MakeObject();
        _ich++; // {
        _SkipSpace();
        if (_ich < _str.size() && _str[_ich] == '}')
        {
            _ich++;
            *pjson = std::move(json);
            return true;
        }
        while (true)
        {
            std::string strKey;
            Json jsonValue;
            _SkipSpace();
            if (_ich >= _str.size() || _str[_ich] != '"')
                return _Fail("expected string key");
            if (!_FParseString(&strKey))
                return false;
            _SkipSpace();
            if (_ich >= _str.size() || _str[_ich] != ':')
                return _Fail("expected ':'");
            _ich++;
            _SkipSpace();
            if (!_FParseValue(&jsonValue, cNest + 1))
                return false;
            json.Set(strKey, std::move(jsonValue));
            _SkipSpace();
            if (_ich >= _str.size())
                return _Fail("unterminated object");
            char ch = _str[_ich++];
            if (ch == '}')
                break;
            if (ch != ',')
                return _Fail("expected ',' or '}'");
        }
        *pjson = std::move(json);
        return true;
    }
};

bool Json::FParse(const std::string &str, Json *pjson, std::string *pstrError)
{
    JsonParser parser(str);
    return parser.FParseDocument(pjson, pstrError);
}

} // namespace automation
