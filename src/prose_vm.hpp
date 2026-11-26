#pragma once
#include "draft.hpp"
#include <functional>
#include <memory>

namespace gpt::draft {
struct Table;
struct Function;
using Value = std::variant<std::monostate, bool, int64_t, double, std::string, Table *, Function *>;
using Values = std::vector<Value>;
struct Object {
    bool marked = false;
    virtual ~Object() = default;
};
struct Cell : Object {
    Value value;
};
struct Table : Object {
    std::map<Value, Value> entries;
    Table *meta = nullptr;
};
struct Function : Object {
    const Proto *body = nullptr;
    std::vector<Cell *> captured;
    Values references;
    std::function<Values(const Values &)> builtin;
};
struct Frame {
    Function *fn;
    Values regs, varargs;
    std::map<unsigned, Cell *> open;
    std::vector<unsigned> closing;
    size_t top = 0;
    Value &slot(unsigned n);
    void close(unsigned n);
};
struct Exit {
    int status;
};
class VM {
    const Program &program;
    std::vector<std::unique_ptr<Object>> heap;
    std::vector<Frame *> frames;
    std::vector<const Values *> call_arguments;
    size_t next_collection = 1024;
    unsigned native_depth = 0;
    void collect();
    void libraries(const std::vector<std::string> &);
    Values execute(Function *, const Values &);

  public:
    Table *globals, *loaded, *strings;
    explicit VM(const Program &, const std::vector<std::string> &);
    template <class T> T *make() {
        auto item = std::make_unique<T>();
        auto ptr = item.get();
        heap.push_back(std::move(item));
        return ptr;
    }
    Function *root(const Proto &);
    Function *native(std::function<Values(const Values &)> fn);
    Values call(Value fn, const Values &args);
    Value get(Value table, Value key);
    void set(Value table, Value key, Value value);
    Value metamethod(const Value &, const std::string &);
    Values module(const std::string &);
    void start(const Values &);
};
bool truth(const Value &);
bool equal(const Value &, const Value &);
std::string show(const Value &);
std::string type_name(const Value &);
int64_t integer(const Value &);
double real(const Value &);
Value at(const Values &, size_t);
Value raw_get(Table *, Value);
void raw_set(Table *, Value, Value);
int64_t length(Table *);
} // namespace gpt::draft
