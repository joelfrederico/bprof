#include <Python.h>
#include <frameobject.h>

#include <chrono>
#include <iostream>
#include <stack>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

using duration = std::chrono::nanoseconds;

std::string PyCode_GetName(PyCodeObject* code) {
  Py_ssize_t size;
  const char* method_name_char = PyUnicode_AsUTF8AndSize(code->co_name, &size);
  return std::string(method_name_char, size);
}
std::string PyFrame_GetName(PyFrameObject* frame) {
  return PyCode_GetName(frame->f_code);
}

static int profile_func(PyObject*, PyFrameObject*, int, PyObject*);
static int trace_func(PyObject*, PyFrameObject*, int, PyObject*);

class BaseFunction {
 public:
  BaseFunction(std::string name) : name_(std::move(name)) {}

  const std::string& name() const { return name_; }
  void add_elapsed_internal(const std::chrono::nanoseconds& time);
  const auto& overhead() const { return internal_time_; }

  void add_call() { ++n_calls_; }
  size_t n_calls() const { return n_calls_; }

 private:
  size_t n_calls_ = 0;
  std::string name_;
  std::chrono::nanoseconds internal_time_ = duration(0);
};

class LineState {
 public:
  void add_internal(const duration& dur) { internal_ += dur; }
  void add_external(const duration& dur) { external_ += dur; }

  const duration& internal() const { return internal_; }
  const duration& external() const { return external_; }

  void add_call() { ++n_calls_; }
  size_t n_calls() const { return n_calls_; }

  LineState& operator+=(const LineState& rhs) {
    n_calls_ += rhs.n_calls_;
    internal_ += rhs.internal_;
    external_ += rhs.external_;
    return *this;
  }

 private:
  size_t n_calls_ = 0;
  duration internal_ = duration(0);
  duration external_ = duration(0);
};

class LineRecord : public LineState {
 public:
  LineRecord(std::string line) : line_(std::move(line)) {}
  LineRecord(LineState state, std::string line)
      : LineState(std::move(state)), line_(std::move(line)) {}

  const std::string& text() const { return line_; }

 private:
  std::string line_;
};

class Function : public BaseFunction {
 public:
  Function(std::string name, std::vector<std::string> lines, PyCodeObject*);

  PyCodeObject* const code() const noexcept { return code_; }
  size_t n_lines() const { return lines_.size(); }
  const auto& lines() const { return lines_; }

  LineRecord& line(size_t line_number) { return lines_.at(line_number); }

 private:
  PyCodeObject* code_;
  std::vector<LineRecord> lines_;
};

void BaseFunction::add_elapsed_internal(const duration& time) {
  internal_time_ += time;
}

Function::Function(
    std::string name, std::vector<std::string> lines, PyCodeObject* code)
      : BaseFunction(std::move(name)), code_(code) {
  auto n_lines = lines_.size();
  lines_.reserve(n_lines);
  for (auto&& line : lines) {
    lines_.emplace_back(line);
  }
}

enum class Instruction {
  kOrigin,
  kLine,
  kCall,
  kReturn,
  kException,
  kCCall,
  kCReturn,
  kCException,
  kInvalid,
};

class FrameState {
 public:
  FrameState(PyCodeObject* code, size_t n_lines, size_t starting_line) 
      : starting_line_(starting_line), function_key_(code) {
    lines_.resize(n_lines);
  }
  PyCodeObject* key() const { return function_key_; }

  duration total_time() const;
  LineState& current_line() { return lines_.at(current_line_-starting_line_-1); }
  LineState& set_current_line(size_t line_number) {
    current_line_ = line_number;
    return current_line();
  }

  void add_internal(const duration& dur) { internal_ += dur; }
  const duration& internal() const { return internal_; }

  const auto& lines() const { return lines_; }

 private:
  size_t starting_line_ = 0;
  size_t current_line_ = 0;
  PyCodeObject* function_key_;
  std::vector<LineState> lines_;
  duration internal_ = duration(0);
};

duration FrameState::total_time() const {
  auto result = duration(0);
  for (auto&& line : lines_) {
    result += line.internal();
    result += line.external();
  }
  return result;
}

class Module {
 public:
  using clock = std::chrono::high_resolution_clock;
  using Time = clock::rep;
  using time_point = clock::time_point;
  Module(PyObject*);
  ~Module();
  void start();
  void stop();
  PyObject* dump(const char*);

  void profile(int what, PyFrameObject* frame, PyObject* arg);
  void profile_call(PyFrameObject*);
  void profile_return(PyFrameObject*);
  void profile_c_call(PyFrameObject*, PyObject*);
  void profile_c_return(PyFrameObject*);
  void profile_line(PyFrameObject*);

  void finish_origin(PyFrameObject*);
  void finish_line(PyFrameObject*);
  void finish_call(PyFrameObject*);
  void finish_return(PyFrameObject*);
  void finish_exception(PyFrameObject*);
  void finish_ccall(PyFrameObject*);
  void finish_creturn(PyFrameObject*);
  void finish_cexception(PyFrameObject*);

  void emplace_frame(PyFrameObject*);
  void pop_frame();

  Function& add_function(PyFrameObject*);
  BaseFunction& add_c_function(std::string);

  duration elapsed();
  const auto& functions() const { return functions_; }
  const auto& c_functions() const { return c_functions_; }

 private:
  std::vector<std::string> get_lines(
      PyFrameObject* lines, size_t* line_start=nullptr);

  PyObject* parent_;
  std::unordered_map<PyCodeObject*, Function> functions_;
  std::unordered_map<std::string, BaseFunction> c_functions_;
  std::stack<FrameState> frame_stack_;
  PyObject* inspect_;
  Instruction last_instruction_ = Instruction::kInvalid;
  time_point last_instruction_start_;
  time_point last_instruction_end_;
  std::string last_c_name_;
};

Module::Module(PyObject* m) : parent_(m) {
  inspect_ = PyImport_ImportModule("inspect");
  if (inspect_ == NULL) {
    throw std::runtime_error("Could not import `inspect'");
  }
}

Module::~Module() {
  if (inspect_ != NULL) {
    Py_DECREF(inspect_);
  } else {
    throw std::runtime_error("Bad");
  }
}

duration Module::elapsed() {
  return std::chrono::duration_cast<duration>(
      last_instruction_end_ - last_instruction_start_);
}

void Module::emplace_frame(PyFrameObject* frame) {
  size_t starting_line = 0;
  auto lines = get_lines(frame, &starting_line);
  frame_stack_.emplace(frame->f_code, lines.size(), starting_line);
}

void Module::start() {
  last_instruction_ = Instruction::kOrigin;

  PyEval_SetProfile(profile_func, parent_);
  PyEval_SetTrace(trace_func, parent_);
}

void Module::finish_origin(PyFrameObject* frame) {
}

void Module::stop() {
  PyEval_SetProfile(NULL, NULL);
  PyEval_SetTrace(NULL, NULL);
}

PyObject* CreateFunctionDict(const BaseFunction& function) {
  PyObject* function_py = PyDict_New();
  PyObject* n_calls = PyLong_FromUnsignedLongLong(function.n_calls());
  const auto& name = function.name();
  PyObject* name_py = PyUnicode_DecodeUTF8(name.data(), name.size(), NULL);
  PyObject* internal = PyLong_FromUnsignedLongLong(function.overhead().count());
  
  PyDict_SetItemString(function_py, "name", name_py);
  Py_DECREF(name_py);
  PyDict_SetItemString(function_py, "n_calls", n_calls);
  Py_DECREF(n_calls);
  PyDict_SetItemString(function_py, "internal_ns", internal);
  Py_DECREF(internal);

  return function_py;
}

PyObject* Module::dump(const char* path) {
  PyObject* functions = PyDict_New();
  for (auto&& function_pair : functions_) {
    Function& function = function_pair.second;
    PyObject* function_py = CreateFunctionDict(function);

    const auto& lines = function.lines();
    PyObject* lines_py = PyList_New(lines.size());
    size_t j = 0;
    for (auto&& line : lines) {
      const auto& line_str = line.text();
      PyObject* line_dict = PyDict_New();
      PyObject* line_str_py = 
	PyUnicode_DecodeUTF8(line_str.data(), line_str.size(), NULL);
      PyObject* line_n_calls = PyLong_FromUnsignedLongLong(line.n_calls());
      PyObject* line_internal =
	PyLong_FromUnsignedLongLong(line.internal().count());
      PyObject* line_external =
	PyLong_FromUnsignedLongLong(line.external().count());

      PyDict_SetItemString(line_dict, "line_str", line_str_py);
      Py_DECREF(line_str_py);
      PyDict_SetItemString(line_dict, "n_calls", line_n_calls);
      Py_DECREF(line_n_calls);
      PyDict_SetItemString(line_dict, "internal_ns", line_internal);
      Py_DECREF(line_internal);
      PyDict_SetItemString(line_dict, "external_ns", line_external);
      Py_DECREF(line_external);

      PyList_SET_ITEM(lines_py, j++, line_dict);
    }
    PyDict_SetItemString(function_py, "lines", lines_py);
    Py_DECREF(lines_py);

    PyObject* key = PyLong_FromUnsignedLongLong(
	reinterpret_cast<size_t>(function_pair.first));
    PyDict_SetItem(functions, key, function_py);
    Py_DECREF(key);
    Py_DECREF(function_py);
  }

  PyObject* c_functions = PyDict_New();
  for (auto&& function_pair : c_functions_) {
    PyObject* function_py = CreateFunctionDict(function_pair.second);
    auto& name_str = function_pair.first;
    PyObject* name = PyUnicode_DecodeUTF8(name_str.data(), name_str.size(), NULL);
    PyDict_SetItem(c_functions, name, function_py);
    Py_DECREF(name);
    Py_DECREF(function_py);
  }

  PyObject* result = PyDict_New();
  PyDict_SetItemString(result, "functions", functions);
  Py_DECREF(functions);
  PyDict_SetItemString(result, "c_functions", c_functions);
  Py_DECREF(c_functions);

  return result;
}

void Module::profile(int what, PyFrameObject* frame, PyObject* arg) {
  last_instruction_end_ = clock::now();

  switch (last_instruction_) {
    case Instruction::kOrigin:
      finish_origin(frame);
      break;
    case Instruction::kLine:
      finish_line(frame);
      break;
    case Instruction::kCall:
      finish_call(frame);
      break;
    case Instruction::kReturn:
      finish_return(frame);
      break;
    case Instruction::kException:
      break;
    case Instruction::kCCall:
      finish_ccall(frame);
      break;
    case Instruction::kCReturn:
      finish_creturn(frame);
      break;
    case Instruction::kCException:
      break;
    case Instruction::kInvalid:
      break;
    default:
      throw std::runtime_error("Should not get here");
  }

  switch (what) {
    case PyTrace_LINE:
      profile_line(frame);
      break;
    case PyTrace_CALL:
      profile_call(frame);
      break;
    case PyTrace_RETURN:
      profile_return(frame);
      break;
    case PyTrace_C_CALL:
      profile_c_call(frame, arg);
      break;
    case PyTrace_C_RETURN:
      profile_c_return(frame);
      break;
    case PyTrace_EXCEPTION:
      break;
    case PyTrace_C_EXCEPTION:
      profile_c_return(frame);
      break;
    case PyTrace_OPCODE:
      break;
    default:
      throw std::runtime_error("Should not get here");
  }
  last_instruction_start_ = clock::now();
}

void Module::profile_call(PyFrameObject* frame) {
  auto& function = add_function(frame);
  function.add_call();
  frame->f_trace_opcodes = 0;
  emplace_frame(frame);
  last_instruction_ = Instruction::kCall;
}

void Module::finish_call(PyFrameObject* frame) {
  functions_.at(frame->f_code).add_elapsed_internal(elapsed());
}

void Module::profile_line(PyFrameObject* frame) {
  last_instruction_ = Instruction::kLine;

  if (frame_stack_.empty()) {
    return;
  }

  auto line_number = PyFrame_GetLineNumber(frame);
  auto& line = frame_stack_.top().set_current_line(line_number);
  line.add_call();
}

void Module::profile_c_call(PyFrameObject* frame, PyObject* arg) {
  PyObject* module = PyObject_GetAttrString(arg, "__module__");
  PyObject* qualname = PyObject_GetAttrString(arg, "__qualname__");
  PyObject* name = PyUnicode_FromFormat("<C-function %U.%U>", module, qualname);
  if (name == NULL) {
    throw std::runtime_error("Could not get C call name");
  }

  Py_ssize_t size;
  const char* name_char = PyUnicode_AsUTF8AndSize(name, &size);
  last_c_name_ = std::string(name_char, size);
  auto& c_function = add_c_function(last_c_name_);
  c_function.add_call();
  last_instruction_ = Instruction::kCCall;

  Py_DECREF(name);
  Py_DECREF(qualname);
  Py_DECREF(module);
}

void Module::finish_ccall(PyFrameObject* frame) {
  c_functions_.at(last_c_name_).add_elapsed_internal(elapsed());
  frame_stack_.top().current_line().add_external(elapsed());
}

void Module::finish_line(PyFrameObject* frame) {
  if (frame_stack_.empty()) {
    return;
  }
  frame_stack_.top().current_line().add_internal(elapsed());
}

void Module::profile_return(PyFrameObject* frame) {
  last_instruction_ = Instruction::kReturn;
}

void Module::finish_return(PyFrameObject*) {
  frame_stack_.top().add_internal(elapsed());
  pop_frame();
}

void Module::profile_c_return(PyFrameObject* frame) {
  last_instruction_ = Instruction::kCReturn;
}

void Module::finish_creturn(PyFrameObject*) {
  frame_stack_.top().add_internal(elapsed());
}

void Module::pop_frame() {
  FrameState& frame = frame_stack_.top();
  Function& function = functions_.at(frame.key());
  function.add_elapsed_internal(frame.internal());
  size_t i = 0;
  for (auto&& line : frame.lines()) {
    LineRecord& line_record = function.line(i++);
    line_record += line;
  }
  auto total = frame.total_time();

  frame_stack_.pop();

  if (!frame_stack_.empty()) {
    frame_stack_.top().current_line().add_external(total);
  }
}

std::vector<std::string> Module::get_lines(
    PyFrameObject* frame, size_t* line_start) {
  PyCodeObject* code = frame->f_code;
  PyObject* method_name_py = PyUnicode_FromString("getsourcelines");
  if (method_name_py == NULL) {
    throw std::runtime_error("Could not create str");
  }

  PyObject* result = PyObject_CallMethodObjArgs(inspect_, method_name_py, (PyObject*)code, NULL);
  if (result == NULL) {
    Py_DECREF(method_name_py);
    throw std::runtime_error("Could not get `inspect.getsourcelines' method");
  }
  PyObject* lines_py = PyTuple_GetItem(result, 0);

  if (line_start != nullptr) {
    PyObject* line_start_py = PyTuple_GetItem(result, 1);
    *line_start = PyLong_AsUnsignedLongLong(line_start_py);
  }

  auto n_lines = PyList_Size(lines_py);
  std::vector<std::string> lines;
  lines.reserve(n_lines);
  for (decltype(n_lines) i=1; i < n_lines; ++i) {
    PyObject* line_py = PyList_GetItem(lines_py, i);
    Py_ssize_t size;
    const char* line_char = PyUnicode_AsUTF8AndSize(line_py, &size);
    lines.emplace_back(std::string(line_char, size));
  }
  Py_DECREF(result);
  Py_DECREF(method_name_py);
  return lines;
}

Function& Module::add_function(PyFrameObject* frame) {
  PyCodeObject* code = frame->f_code;
  if (functions_.count(code) != 0) {
    return functions_.at(code);
  }
  auto lines = get_lines(frame);
  auto pair = 
    functions_.emplace(
	code, Function(PyFrame_GetName(frame), std::move(lines), code));
  return pair.first->second;
}

BaseFunction& Module::add_c_function(std::string name) {
  auto pair = c_functions_.emplace(name, BaseFunction(name));
  return pair.first->second;
}

static int
profile_func(PyObject* obj, PyFrameObject* frame, int what, PyObject *arg) {
  Module* mod = (Module*)PyModule_GetState(obj);
  mod->profile(what, frame, arg);
  return 0;
}

static int
trace_func(PyObject* obj, PyFrameObject* frame, int what, PyObject *arg) {
  // We are uninterested in these events
  if (what != PyTrace_LINE) {
    return 0;
  }
  return profile_func(obj, frame, what, arg);
}

static int
module_exec(PyObject *m)
{
  new(PyModule_GetState(m)) Module(m);
  return 0;
}

static PyObject*
module_start(PyObject* m, PyObject*) {
  Module* mod = (Module*)PyModule_GetState(m);
  mod->start();
  Py_RETURN_NONE;
}

static PyObject*
module_stop(PyObject* m, PyObject*) {
  Module* mod = (Module*)PyModule_GetState(m);
  mod->stop();
  Py_RETURN_NONE;
}

static PyObject*
module_dump(PyObject* m, PyObject* path) {
  Module* mod = (Module*)PyModule_GetState(m);

  PyObject* bytes;
  if (!PyArg_ParseTuple(path, "O&", PyUnicode_FSConverter, &bytes)) {
    return NULL;
  }

  char* bytes_data = PyBytes_AsString(bytes);
  if (bytes_data == NULL) {
    return NULL;
  }

  return mod->dump(bytes_data);
}

PyDoc_STRVAR(module_doc,
"This is the C++ implementation.");

static PyMethodDef module_methods[] = {
    {"start", module_start, METH_NOARGS,
        PyDoc_STR("start() -> None")},
    {"stop", module_stop, METH_NOARGS,
        PyDoc_STR("stop() -> None")},
    {"dump", module_dump, METH_VARARGS,
        PyDoc_STR("dump() -> None")},
    {NULL,              NULL}           /* sentinel */
};

static void module_free(void* m) {
  if (m == NULL) {
    return;
  }
  Module* mod = (Module*)PyModule_GetState((PyObject*)m);
  if (mod != NULL) {
    mod->~Module();
  }
}

static struct PyModuleDef_Slot module_slots[] = {
  {Py_mod_exec, (void*)module_exec},
  {0, NULL},
};

static struct PyModuleDef module = {
  PyModuleDef_HEAD_INIT,
  "_bprof",
  module_doc,
  sizeof(Module),
  module_methods,
  module_slots,
  NULL,
  NULL,
  module_free
};

PyMODINIT_FUNC
PyInit__bprof(void)
{
  return PyModuleDef_Init(&module);
}
