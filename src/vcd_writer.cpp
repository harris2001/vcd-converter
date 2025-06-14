#include <string>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <array>
#include <list>
#include <utility>
#include "vcd_writer.h"


// -----------------------------
namespace vcd {
namespace utils {
void replace_new_lines(std::string &str, const std::string &sub);
}
using namespace utils;

// -----------------------------
struct VCDHeader final
{
    enum KW { KW_TIMESCALE, KW_DATE, KW_COMMENT, KW_VERSION, KW_COUNT_ };
    static constexpr std::array<const char*, KW_COUNT_> kw_names{ "$timescale", "$date", "$comment", "$version" };

    const TimeScale timescale_quan;
    const TimeScaleUnit timescale_unit;
    const std::array<std::string, KW_COUNT_> kw_values;

    VCDHeader() = delete;
    VCDHeader(VCDHeader&&) = default;
    VCDHeader(const VCDHeader&) = delete;
    VCDHeader& operator=(const VCDHeader&) = delete;
    VCDHeader& operator=(VCDHeader&&) = delete;
    ~VCDHeader() = default;
    VCDHeader(TimeScale     timescale_quan,
              TimeScaleUnit timescale_unit,
              const std::string& date,
              const std::string& comment,
              const std::string& version) :
        timescale_quan{ timescale_quan },
        timescale_unit{ timescale_unit },
        kw_values{ timescale(), date, comment, version }
    {}

private:
    [[nodiscard]]std::string timescale() const
    {
        if(timescale_quan >= TimeScale::_COUNT_)
            throw VCDTypeException{ format("Invalid time scale quant %d", timescale_quan) };
        if(timescale_unit >= TimeScaleUnit::_count_)
            throw VCDTypeException{ format("Invalid time scale unit %d", timescale_unit) };
        if(!utils::validate_date(kw_values[KW_DATE]))
            throw VCDTypeException{ format("Invalid date '%s' format", kw_values[KW_DATE].c_str()) };

        const std::array<const char*, int(TimeScaleUnit::_count_)> TIMESCALE_UNITS{ "s", "ms", "us", "ns", "ps", "fs" };
        return std::to_string(int(timescale_quan)) + " " + TIMESCALE_UNITS[int(timescale_unit)];
    }
};

// -----------------------------
HeadPtr makeVCDHeader(TimeScale timescale_quan, TimeScaleUnit timescale_unit, const std::string &date,
                      const std::string &comment, const std::string &version)
{
    return HeadPtr{ new VCDHeader(timescale_quan, timescale_unit, date, comment, version) };
}

// -----------------------------
void VCDHeaderDeleter::operator()(VCDHeader *p) { delete p; }

// -----------------------------
struct VCDScope final
{
    std::string name;
    ScopeType   type;
    std::list<VarPtr> vars;

    VCDScope(std::string_view name, ScopeType type) : 
        name(name), type(type) {}
};

// -----------------------------
bool ScopePtrHash::operator()(const ScopePtr &l, const ScopePtr &r) const
{
    return (l->name < r->name);
}

// -----------------------------
// VCD variable details needed to call :meth:`VCDWriter.change()`.
class VCDVariable
{
public:
    VCDVariable() = delete;
    VCDVariable& operator=(VCDVariable&&) = delete;

private:
    VCDVariable(VCDVariable&&) = default;

protected:
    VCDVariable(std::string name, VariableType type, unsigned size, ScopePtr scope, unsigned next_var_id);

public:
    VCDVariable(const VCDVariable&) = delete;
    VCDVariable& operator=(const VCDVariable&) = delete;

    unsigned    _ident;  // internal ID used in VCD output stream
    VariableType _type;  // VCD variable type, one of `VariableTypes`
    std::string  _name;  // human-readable name
    unsigned     _size;  // size of variable, in bits
    std::weak_ptr<VCDScope>    _scope;  // pointer to scope string

    //! string representation of variable types
    static const std::array<std::string, 20> VAR_TYPES;

public:
    virtual ~VCDVariable() = default;

    //! string representation of variable declartion in VCD
    [[nodiscard]] std::string declartion() const;
    //! string representation of value change record in VCD
    [[nodiscard]] virtual VarValue change_record(const VarValue &value) const = 0;

    friend class VCDWriter;
    friend struct VarPtrHash;
    friend struct VarPtrEqual;
};

// -----------------------------
const std::array<std::string, 20> VCDVariable::VAR_TYPES = { 
    "wire", "reg", "string", "parameter", "integer", "real", "realtime", "time", "event",
    "supply0", "supply1", "tri", "triand", "trior", "trireg", "tri0", "tri1", "wand", "wor"
};

// -----------------------------
size_t VarPtrHash::operator()(const VarPtr &p) const
{
    std::hash<std::string> h;
    std::shared_ptr<VCDScope> scope = p->_scope.lock();
    return (h(p->_name) ^ (h(scope->name) << 1));
}

// -----------------------------
bool VarPtrEqual::operator()(const VarPtr &a, const VarPtr &b) const
{
    std::shared_ptr<VCDScope> scope_a = a->_scope.lock();
    std::shared_ptr<VCDScope> scope_b = b->_scope.lock();
    return (a->_name == b->_name) && (scope_a->name == scope_b->name);
}

// -----------------------------
// One-bit VCD scalar is a 4-state variable and thus may have one of
// `VCDValues`. An empty *value* is the same as `VCDValues::UNDEF`
struct VCDScalarVariable : public VCDVariable
{
    VCDScalarVariable(const std::string &name, VariableType type, unsigned size, ScopePtr scope, unsigned next_var_id) :
        VCDVariable(name, type, size, std::move(scope), next_var_id)
    {}
    [[nodiscard]] VarValue change_record(const VarValue &value) const override
    {
        char c = (value.size())? char(tolower(value[0])) : char(VCDValues::UNDEF);
        if (value.size() != 1 || (c != VCDValues::ONE   && c != VCDValues::ZERO
                               && c != VCDValues::UNDEF && c != VCDValues::HIGHV))
            throw VCDTypeException{ format("Invalid scalar value '%c'", c) };
        return {c};
    }
};

// -----------------------------
// String variable as known by GTKWave. Any `string` (character-chain) 
// can be displayed as a change.This type is only supported by GTKWave.
struct VCDStringVariable : public VCDVariable
{
    VCDStringVariable(const std::string &name, VariableType type, unsigned size, ScopePtr scope, unsigned next_var_id) :
        VCDVariable(name, type, size, std::move(scope), next_var_id)
    {}
    [[nodiscard]]VarValue change_record(const VarValue &value) const override
    {
        if (value.find(' ') != std::string::npos)
            throw VCDTypeException{ format("Invalid string value '%s'", value.c_str()) };
        return format("s%s ", value.c_str());
    }
};

// -----------------------------
// Real (IEEE-754 double-precision floating point) variable. Values must
// be numeric and can't be `VCDValues::UNDEF` or `VCDValues::HIGHV` states
struct VCDRealVariable : public VCDVariable
{
    VCDRealVariable(const std::string &name, VariableType type, unsigned size, ScopePtr scope, unsigned next_var_id) :
        VCDVariable(name, type, size, std::move(scope), next_var_id) {}
    [[nodiscard]]std::string change_record(const VarValue &value) const override
    { return format("r%.16g ", stod(value)); }
};

// -----------------------------
// Bit vector variable type for the various non-scalar and non-real 
// variable types, including integer, register, wire, etc.
struct VCDVectorVariable : public VCDVariable
{
    VCDVectorVariable(const std::string &name, VariableType type, unsigned size, ScopePtr scope, unsigned next_var_id) :
        VCDVariable(name, type, size, std::move(scope), next_var_id) {}
    [[nodiscard]]std::string change_record(const VarValue &value) const override;
};

// -----------------------------
struct VarSearch final
{
    VCDScope vcd_scope = { "", ScopeType::module };
    ScopePtr ptr_scope = { &vcd_scope, [](VCDScope*) {} };
    VCDScalarVariable vcd_var = { "", VCDWriter::var_def_type, 0, ptr_scope, 0 };
    VarPtr ptr_var = { &vcd_var, [](VCDScalarVariable*) {} };

    VarSearch() = delete;
    VarSearch(ScopeType scope_def_type) : vcd_scope("", scope_def_type) {}
};

// -----------------------------
VCDWriter::VCDWriter(std::string filename, HeadPtr &header, unsigned init_timestamp) :
    _timestamp(init_timestamp),
    _header((header) ? std::move(header) : makeVCDHeader()),
    _scope_sep("."),
    _scope_def_type(ScopeType::module),
    _filename(std::move(filename)),
    _dumping(true),
    _registering(true),
    _search(std::make_shared<VarSearch>(_scope_def_type)),
    _ofile(fmt::output_file(_filename))
{
    if (!_header)
        throw VCDTypeException{ "Invalid pointer to header" };
    
}

// -----------------------------
VarPtr VCDWriter::register_var(const std::string &scope, const std::string &name, VariableType type,
                               unsigned size, const VarValue &init, bool duplicate_names_check)
{
    VarPtr pvar;
    if (_closed)
        throw VCDPhaseException{ "Cannot register after close()" };
    if (!_registering)
        throw VCDPhaseException{ format("Cannot register new var '%s', registering finished", name.c_str()) };

    if (scope.size() == 0 || name.size() == 0)
        throw VCDTypeException{ format("Empty scope '%s' or name '%s'", scope.c_str(), name.c_str()) };

    _search->vcd_scope.name = scope;
    auto cur_scope = _scopes.find(_search->ptr_scope);
    if (cur_scope == _scopes.end())
    {
        auto res = _scopes.insert(std::make_shared<VCDScope>(scope, _scope_def_type));
        if (!res.second)
            throw VCDPhaseException{ format("Cannot insert scope '%s'", scope.c_str()) };
        cur_scope = res.first;
    }

    auto sz = [&size](unsigned def) { return (size ? size : def);  };

    VarValue init_value(init);
    switch (type)
    {
        case VariableType::integer:   
        case VariableType::realtime:
            if (sz(64) == 1)
                pvar = VarPtr(new VCDScalarVariable(name, type, 1, *cur_scope, _next_var_id));
            else
                pvar = VarPtr(new VCDVectorVariable(name, type, sz(64), *cur_scope, _next_var_id));
            break;

        case VariableType::real:
            pvar = VarPtr(new VCDRealVariable(name, type, sz(64), *cur_scope, _next_var_id));
            if (init_value.size() == 1 && init_value[0] == VCDValues::UNDEF)
                init_value = "0.0";
            break;

        case VariableType::string:
            pvar = VarPtr(new VCDStringVariable(name, type, sz(1), *cur_scope, _next_var_id));
            break;

        case VariableType::event:
            pvar = VarPtr(new VCDScalarVariable(name, type, 1, *cur_scope, _next_var_id));
            break;

        default:
            if (!size)
                throw VCDTypeException{ format("Must supply size for type '%s' of var '%s'",
                                               VCDVariable::VAR_TYPES[(int)type].c_str(), name.c_str()) };

            pvar = VarPtr(new VCDVectorVariable(name, type, size, *cur_scope, _next_var_id));
            if (init_value.size() == 1 && init_value[0] == VCDValues::UNDEF)
                init_value = std::string(size, VCDValues::UNDEF);
            break;
    }     
    if (type != VariableType::event)
        _change(pvar, _timestamp, init_value, true);
    
    if (duplicate_names_check && _vars.find(pvar) != _vars.end())
        throw VCDTypeException{ format("Duplicate var '%s' in scope '%s'", name.c_str(), scope.c_str()) };

    _vars.insert(pvar);
    (**cur_scope).vars.push_back(pvar);
    // Only alter state after change_func() succeeds
    _next_var_id++;
    return pvar;
}

// -----------------------------
bool VCDWriter::_change(VarPtr var, TimeStamp timestamp, const VarValue &value, bool reg)
{
    if (timestamp < _timestamp)
        throw VCDPhaseException{ format("Out of order value change var '%s'", var->_name.c_str()) };
    else if (_closed)
        throw VCDPhaseException{ "Cannot change value after close()" };

    if (!var)
        throw VCDTypeException{ "Invalid VCDVariable" };

    if (timestamp > _timestamp)
    {
        if (_registering)
            _finalize_registration();
        if (_dumping)
            _ofile.print("#{:d}\n", timestamp);
        _timestamp = timestamp;
    }

    VarValue change_value = var->change_record(value);
    // if value changed
    auto it = _vars_prevs.find(var);
    if (it != _vars_prevs.end())
    {
        if (it->second == change_value)
            return false;
        it->second = change_value;
    }
    else
    {
        if (!reg)
            throw VCDTypeException{ format("VCDVariable '%s' do not registered", var->_name.c_str()) };
        else
            _vars_prevs.emplace(var, change_value);
    }
    // dump it into file
    if (_dumping && !_registering)
        _ofile.print("{:s}{:x}\n", change_value.c_str(), var->_ident);
    return true;
}

// -----------------------------
bool VCDWriter::change(const std::string &scope, const std::string &name, TimeStamp timestamp, const VarValue &value)
{
    return _change(var(scope, name), timestamp, value, false);
}

// -----------------------------
VarPtr VCDWriter::var(const std::string &scope, const std::string &name) const
{
    // The _search is a speed optimisation
    _search->vcd_scope.name = scope;
    _search->vcd_var._name = name;
    auto it_var = _vars.find(_search->ptr_var);
    if (it_var == _vars.end())
        throw VCDPhaseException{ format("The var '%s' in scope '%s' does not exist", name.c_str(), scope.c_str()) };
    return *it_var;
}

// -----------------------------
void VCDWriter::set_scope_type(std::string &scope, ScopeType scope_type)
{
    // The _search is a speed optimisation
    _search->vcd_scope.name = scope;
    auto it = _scopes.find(_search->ptr_scope);
    if (it == _scopes.end())
        throw VCDPhaseException{ format("Such scope '%s' does not exist", scope.c_str()) };
    (**it).type = scope_type;
}


// -----------------------------
void VCDWriter::_dump_off(TimeStamp timestamp)
{
    _ofile.print("#{:d}\n", timestamp);
    _ofile.print("$dumpoff\n");
    for (const auto &p : _vars_prevs)
    {
        const auto ident = p.first->_ident;
        const char *value = p.second.c_str();

        if (value[0] == 'r')
        {} // real variables cannot have "z" or "x" state
        else if (value[0] == 'b')
        { _ofile.print("bx {:x}\n", ident); }
        //else if (value[0] == 's')
        //{ _ofile.print("sx %x\n", ident); }
        else
        { _ofile.print("x{:x}\n", ident); }
    }
    _ofile.print("$end\n");
}

// -----------------------------
void VCDWriter::_dump_values(const char *keyword)
{
    _ofile.print("{:s}\n", keyword);
    if(!_dumping)
        return;
    // TODO : events should be excluded
    for (const auto &p : _vars_prevs)
    {
        const auto ident = p.first->_ident;
        const char *value = p.second.c_str();
        _ofile.print("{:s}{:x}\n", value, ident);
    }
    _ofile.print("$end\n");
}

// -----------------------------
void VCDWriter::_scope_declaration(const std::string &scope, ScopeType type, size_t sub_beg, size_t sub_end)
{
    const std::array<std::string, 5> SCOPE_TYPES = { "begin", "fork", "function", "module", "task" };

    auto scope_name = scope.substr(sub_beg, sub_end - sub_beg);
    auto scope_type = SCOPE_TYPES[int(type)].c_str();
    _ofile.print("$scope {:s} {:s} $end\n", scope_type, scope_name.c_str());
}

// -----------------------------
void VCDWriter::_write_header()
{
    for (int i = 0; i < VCDHeader::KW_COUNT_; ++i)
    {
        auto kwname = VCDHeader::kw_names[i];
        auto kwvalue = _header->kw_values[i];
        if (kwvalue.empty())
            continue;
        replace_new_lines(kwvalue, "\n\t");
        _ofile.print("{:s} {:s} $end\n", kwname, kwvalue.c_str());
    }

    // nested scope
    size_t n = 0, n_prev = 0;
    std::string scope_prev = "";
    for (auto& s : _scopes) // sorted
    {
        const std::string &scope = s->name;
        // scope print close
        if (scope_prev.size())
        {
            n_prev = 0;
            n = scope_prev.find(_scope_sep);
            n = (n == std::string::npos) ? scope_prev.size() : n;
            // equal prefix
            while (std::strncmp(scope.c_str(), scope_prev.c_str(), n) == 0)
            {
                n_prev = n + _scope_sep.size();
                n = scope_prev.find(_scope_sep, n_prev);
                if (n == std::string::npos)
                    break;
            }
            // last
            if (n_prev != (scope_prev.size() + _scope_sep.size()))
                _ofile.print("$upscope $end\n");
            // close
            n = scope_prev.find(_scope_sep, n_prev);
            while (n != std::string::npos)
            {
                _ofile.print("$upscope $end\n");
                n = scope_prev.find(_scope_sep, n + _scope_sep.size());
            }
        }

        // scope print open
        n = scope.find(_scope_sep, n_prev);
        while (n != std::string::npos)
        {
            _scope_declaration(scope, s->type, n_prev, n);
            n_prev = n + _scope_sep.size();
            n = scope.find(_scope_sep, n_prev);
        }
        // last
        _scope_declaration(scope, s->type, n_prev);

        // dump variable declartion
        for (const auto& var : s->vars)
            _ofile.print("{:s}\n", var->declartion().c_str());

        scope_prev = scope;
    }

    // scope print close (rest)
    if (scope_prev.size())
    {
        // last
        _ofile.print("$upscope $end\n");
        n = scope_prev.find(_scope_sep);
        while (n != std::string::npos)
        {
            _ofile.print("$upscope $end\n");
            n = scope_prev.find(_scope_sep, n + _scope_sep.size());
        }
    }

    _ofile.print("$enddefinitions $end\n");
    // do not need anymore
    _header.reset(nullptr);
}

// -----------------------------
void VCDWriter::_finalize_registration()
{
    assert(_registering);
    _write_header();
    if (_vars_prevs.size())
    {
        _ofile.print("#{:d}\n", _timestamp);
        _dump_values("$dumpvars");
        if (!_dumping)
            _dump_off(_timestamp);
    }
    _registering = false;
}

// -----------------------------
VCDVariable::VCDVariable(std::string name, VariableType type, unsigned size, ScopePtr scope, unsigned next_var_id) :
    _ident(next_var_id), _type(type), _name(std::move(name)), _size(size), _scope(std::move(scope))
{
}

// -----------------------------
std::string VCDVariable::declartion() const
{
    return format("$var %s %d %x %s $end", VAR_TYPES[int(_type)].c_str(), _size, _ident, _name.c_str());
}

// -----------------------------
//  :Warning: *value* is string where all characters must be one of `VCDValues`.
//  An empty  *value* is the same as `VCDValues::UNDEF`
VarValue VCDVectorVariable::change_record(const VarValue &value) const
{
    if (value.size() > _size)
        throw VCDTypeException{ format("Invalid binary vector value '%s' size '%d'", value.c_str(), _size) };

    static VarValue val; // no thread safe mem-alloc optimization
    val.reserve(_size + 2);
    val = ('b' + value + ' ');

    auto val_sz = value.size();
    for (auto i = 1u; i < (val_sz - 1); ++i)
    {
        val[i] = static_cast<char>(tolower(static_cast<unsigned char>(val[i])));
        switch(val[i])
        {
        case VCDValues::ONE:
        case VCDValues::ZERO:
        case VCDValues::UNDEF:
        case VCDValues::HIGHV:
            break;
        default:
            throw VCDTypeException{ format("Invalid binary vector value '%s' size '%d'", val.c_str(), _size) };
        }
    }

    if (!val_sz)
        val = ('b' + std::string(_size, VCDValues::UNDEF) + ' ');
    else if (val_sz < _size) // align
    {
        /***
         * Example: _size = 4, a 4 bit vector
         * value is 'xx' => val_sz = 2
         * then val is 'bxx ' when entering here, but this is not yet alligned
         * end result should look like 'b00xx '
         * so 'xx' needs to be aligned to the right
         * 'b|x|x| |'       val
         * 'b|0|0|x|x| |'   val desired
         * |0|1|2|3|4|5|    index positions
         */

        val.resize(_size + 2); // to allow full bit chars + 'b' in beginning and a space ' ' at the end
        auto k = _size - val_sz;
        /***
        * k: amount that original 'value' input in val (here 'xx') need to shifted
        * or in other words: number of zeros that need to be filled in before 'xx'
        * k is then size(4) - val_sz (2) = 2,
        * as seen in example first 'x' need to be moved from pos 1 -> 3, second from 2 -> 4
        * reverse through the 'xx' string, as otherwise chars might be moved to positions that need to be moved aswell
        * start at val_sz(2) and go back, ending before hitting the 'b'
        */
        for (auto i = val_sz ; i >= 1; --i)
            val[k + i] = val[i];
        // set remaining values zero
        for (auto i = 1u; i <= k; ++i)
            val[i] = VCDValues::ZERO;
        // set last char to a space
        val[_size+1] = ' ';
    }
    return val;
}

// -----------------------------
} //end namespace vcd


