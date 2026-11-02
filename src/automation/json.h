/***************************************************************************
    Automation bridge: a small JSON value type, parser and serializer.

    Self-contained (standard library only) so the bridge adds no build
    dependencies. Numbers are stored as double; integers up to 2^53 round-trip.
    Objects keep insertion order so replies read naturally.
***************************************************************************/
#ifndef AUTOMATION_JSON_H
#define AUTOMATION_JSON_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace automation
{

class Json
{
  public:
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t)
    {
    }
    Json(bool f) : _type(Type::Bool), _bool(f)
    {
    }
    Json(int n) : _type(Type::Number), _number(n)
    {
    }
    Json(long n) : _type(Type::Number), _number(static_cast<double>(n))
    {
    }
    Json(long long n) : _type(Type::Number), _number(static_cast<double>(n))
    {
    }
    Json(unsigned int n) : _type(Type::Number), _number(n)
    {
    }
    Json(unsigned long n) : _type(Type::Number), _number(static_cast<double>(n))
    {
    }
    Json(unsigned long long n) : _type(Type::Number), _number(static_cast<double>(n))
    {
    }
    Json(double d) : _type(Type::Number), _number(d)
    {
    }
    Json(float d) : _type(Type::Number), _number(d)
    {
    }
    Json(const char *psz) : _type(Type::String), _string(psz)
    {
    }
    Json(std::string s) : _type(Type::String), _string(std::move(s))
    {
    }
    Json(Array a) : _type(Type::Array), _array(std::move(a))
    {
    }
    Json(Object o) : _type(Type::Object), _object(std::move(o))
    {
    }

    static Json MakeArray()
    {
        return Json(Array());
    }
    static Json MakeObject()
    {
        return Json(Object());
    }

    Type GetType() const
    {
        return _type;
    }
    bool IsNull() const
    {
        return _type == Type::Null;
    }
    bool IsBool() const
    {
        return _type == Type::Bool;
    }
    bool IsNumber() const
    {
        return _type == Type::Number;
    }
    bool IsString() const
    {
        return _type == Type::String;
    }
    bool IsArray() const
    {
        return _type == Type::Array;
    }
    bool IsObject() const
    {
        return _type == Type::Object;
    }

    bool AsBool() const
    {
        return _bool;
    }
    double AsNumber() const
    {
        return _number;
    }
    const std::string &AsString() const
    {
        return _string;
    }
    const Array &AsArray() const
    {
        return _array;
    }
    const Object &AsObject() const
    {
        return _object;
    }

    // Object helpers. Get() returns a shared null for missing keys or when
    // this is not an object.
    const Json &Get(const std::string &key) const;
    bool Has(const std::string &key) const;
    // Inserts (or replaces) a member; turns a null value into an object.
    Json &Set(const std::string &key, Json value);
    // Appends to an array; turns a null value into an array.
    void Push(Json value);
    size_t Size() const;

    std::string Dump() const;
    void DumpTo(std::string *pstr) const;

    // Parses a complete JSON document. On failure returns false and fills
    // *pstrError with a message including the byte offset.
    static bool FParse(const std::string &str, Json *pjson, std::string *pstrError);

  private:
    Type _type = Type::Null;
    bool _bool = false;
    double _number = 0;
    std::string _string;
    Array _array;
    Object _object;
};

// Appends str as a JSON string literal (including quotes) to *pstrOut.
void JsonQuote(const std::string &str, std::string *pstrOut);

} // namespace automation

#endif // AUTOMATION_JSON_H
