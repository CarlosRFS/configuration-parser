/* State Machine to handle the configuration file. */
#include "statemachine.h"
#include "string-helpers.h"
#include <boost/algorithm/string.hpp>
#include <iostream>
#include <memory>

Q_LOGGING_CATEGORY(parser, "parser")

namespace {
std::shared_ptr<MetaProperty> current_property;
std::shared_ptr<MetaClass> current_class;

std::string global_string;
std::string last_comment;
std::string array_value;
} // namespace

/* reads #include directives. */
callback_t state_include(MetaConfiguration &conf, std::ifstream &f,
                         int &error) {
  char include_name[80];
  char delimiter_begin;
  char delimiter_end;
  bool is_global = false;
  f.get(delimiter_begin);
  if(delimiter_begin == '"') {
    delimiter_begin = '"';
    delimiter_end = '"';
  }
  else {
    delimiter_begin = '<';
    delimiter_end = '>';
    is_global = true;
  }
  f.ignore(256, delimiter_begin);
  f.getline(include_name, 80, delimiter_end); // read untill > or "
  MetaInclude temporary;
  temporary.name = include_name;
  temporary.is_global = is_global;
  conf.includes.push_back(temporary);
  qCDebug(parser) << "include added: " << include_name;
  return initial_state;
}

callback_t multi_purpose_string_state(MetaConfiguration &conf, std::ifstream &f,
                                      int &error) {
  std::vector<char> delimiters = {'{', '[', '=', ' ', '\n'};
  global_string += read_untill_delimiters(f, delimiters);
  boost::trim(global_string);

  qCDebug(parser) << "string found: " << global_string
                  << " next character: " << (char)f.peek();
  if (!conf.top_level_class && global_string == "namespace") {
    conf.conf_namespace = read_untill_delimiters(f, {'{'});
    boost::trim(conf.conf_namespace);

    f.ignore();
    global_string.clear();
    qCDebug(parser) << "Setting a namespace" << conf.conf_namespace;
    return initial_state;
  }

  return current_property ? nullptr // create a state for them.
                          : current_class ? class_state : initial_state;
}

callback_t guess_documentation_state(MetaConfiguration &conf, std::ifstream &f,
                                     int &error) {
  f.ignore();
  char c = f.peek();
  switch (c) {
  case '*':
    return multi_line_documentation_state;
  case '/':
    return single_line_documentation_state;
  }
  return nullptr;
}

callback_t multi_line_documentation_state(MetaConfiguration &conf,
                                          std::ifstream &f, int &error) {
  std::string comment;
  char c;
  while (f.peek() != EOF) {
    c = f.get();
    if (c == '*' && f.peek() == '/') {
      goto exit;
    }
    comment += c;
  }
  last_comment = comment;

exit:
  return current_property ? property_state
                          : current_class ? class_state : initial_state;
}

callback_t single_line_documentation_state(MetaConfiguration &conf,
                                           std::ifstream &f, int &error) {
  return nullptr;
}

callback_t begin_class_state(MetaConfiguration &conf, std::ifstream &f,
                             int &error) {
  qCDebug(parser) << "Starting class: " << global_string;

  f.ignore(); // eat the '{' character.
  clear_empty(f);
  boost::trim(global_string);

  if (!conf.top_level_class) {
    qCDebug(parser) << "Creating the top level class";
    conf.top_level_class = std::make_shared<MetaClass>();
    current_class = conf.top_level_class;
  } else {
    assert(current_class);
    qCDebug(parser) << "Creating a child class of" << current_class->name;
    auto old_parent = current_class;
    current_class->subclasses.push_back(std::make_shared<MetaClass>());
    current_class = current_class->subclasses.back();
    current_class->parent = old_parent;
  }
  current_class->name = global_string;

  qCDebug(parser) << "class found: " << current_class->name
                  << ", value = " << array_value;

  global_string.clear();
  return class_state;
}

callback_t end_class_state(MetaConfiguration &conf, std::ifstream &f,
                           int &error) {
  f.ignore();
  qCDebug(parser) << "finishing class " << current_class->name;
  current_class = current_class->parent;

  if (current_class)
    return class_state;
  return nullptr;
}

callback_t begin_property_state(MetaConfiguration &conf, std::ifstream &f,
                                int &error) {
  // here we know that we have at least the type of the property,
  // but it can be three kinds of string.

  std::string property_name;
  clear_empty(f);

  boost::trim(global_string);
  current_property = std::make_shared<MetaProperty>();

  if (global_string == "enum") {
    current_property->is_enum = true;
    f >> global_string;
    qCDebug(parser) << "Enum with type" << global_string;
  }

  current_property->type = global_string;
  current_property->parent = current_class;
  global_string.clear();

  // find the name of the property
  property_name = read_untill_delimiters(f, {'=', '\n'});
  boost::trim(property_name);
  current_property->name = property_name;
  qCDebug(parser) << "Starting property " << property_name << " ";
  return property_state;
}

callback_t begin_property_set_state(MetaConfiguration &conf, std::ifstream &f,
                                    int &error) {
  std::string name;
  std::string value;
  std::string tmp;
  f.ignore(); // ignoring {

  do {
    f >> name;
    if (name == "}")
      break;

    f >> tmp;
    if (tmp == "}")
      break;

    f >> value;
    if (value == "}")
      break;

    if (name == "value") {
      qCDebug(parser) << "found value for property  " << value;
      current_property->default_value = value;
    } else {
      qCDebug(parser) << "found setter " << name << " with value " << value;
      current_property->setters.insert(std::make_pair(name, value));
    }
  } while (true);
  return end_property_state;
}

callback_t property_state(MetaConfiguration &conf, std::ifstream &f,
                          int &error) {
  while (f.peek() != '\n' && f.peek() != '=' && f.peek() != '[') {
    f.ignore();
  }

  // easy, line finished, next property or class.
  if (f.peek() == '\n') {
    return end_property_state;
  } else if (f.peek() == '=') {
    f.ignore();
    clear_empty(f);
    if (f.peek() == '{') {
      qCDebug(parser) << "starting the set property set";
      return begin_property_set_state;
    } else {
      char buffer[80];
      f.getline(buffer, 80, '\n');
      current_property->default_value = buffer;
      return end_property_state;
    }
  }
  return nullptr;
}

callback_t end_property_state(MetaConfiguration &conf, std::ifstream &f,
                              int &error) {
  current_class->properties.push_back(current_property);
  qCDebug(parser) << "finishing property" << current_property->name;
  current_property = nullptr;
  return class_state;
}

/* Start the default stuff -- classes,  includes and documentation. */
callback_t initial_state(MetaConfiguration &conf, std::ifstream &f,
                         int &error) {
  clear_empty(f);

  char c = f.peek();
  qCDebug(parser) << "Peeking" << c;
  switch (c) {
  case '#':
    return state_include;
  case '{':
    return begin_class_state;
  // case '[' : return begin_array_state;
  case '/':
    return guess_documentation_state;
  case EOF:
    return nullptr;
  default:
    return multi_purpose_string_state;
  }
  return nullptr;
}

/* starts the class stuff */
callback_t class_state(MetaConfiguration &conf, std::ifstream &f, int &error) {
  clear_empty(f);
  char c = f.peek();
  switch (c) {
  case '{':
    return begin_class_state;
  case '}':
    return end_class_state;
    //  case '[' : return begin_array_state;
  case '/':
    return guess_documentation_state;
  }
  return global_string.size() ? begin_property_state
                              : multi_purpose_string_state;
}

MetaConfiguration parse_configuration(std::ifstream &f) {
  int error;
  MetaConfiguration conf;
  callback_t state = initial_state;
  while (state) {
    state = state(conf, f, error);
  }
  return conf;
}
