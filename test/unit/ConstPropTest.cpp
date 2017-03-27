/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include <unordered_map>

#include "Creators.h"
#include "DexAnnotation.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "FinalInline.h"
#include "Resolver.h"
#include "Transform.h"

  // Map of type string -> (sget opcode, sput opcode)
static std::unordered_map<std::string, std::pair<DexOpcode, DexOpcode>> init_ops = {
  {"I", {OPCODE_SGET, OPCODE_SPUT}},
  {"Z", {OPCODE_SGET_BOOLEAN, OPCODE_SPUT_BOOLEAN}},
  {"B", {OPCODE_SGET_BYTE, OPCODE_SPUT_BYTE}},
  {"C", {OPCODE_SGET_CHAR, OPCODE_SPUT_CHAR}},
  {"S", {OPCODE_SGET_SHORT, OPCODE_SPUT_SHORT}}
};

struct ConstPropTest : testing::Test {
  DexType* m_int_type;
  DexType* m_bool_type;
  DexType* m_byte_type;
  DexType* m_char_type;
  DexType* m_short_type;

  ConstPropTest() {
    g_redex = new RedexContext();
    m_int_type = DexType::make_type("I");
    m_bool_type = DexType::make_type("Z");
    m_byte_type = DexType::make_type("B");
    m_char_type = DexType::make_type("C");
    m_short_type = DexType::make_type("S");
  }

  ~ConstPropTest() {
    delete g_redex;
  }

  void expect_empty_clinit(DexClass* clazz) {
    auto clinit = clazz->get_clinit();
    ASSERT_NE(clinit, nullptr) << "Class " << clazz->c_str() << " missing clinit";
    auto code = clinit->get_code();
    EXPECT_EQ(code->get_entries()->count_opcodes(), 0)
        << "Class " << clazz->c_str() << " has non-empty clinit";
  }

  void expect_field_eq(DexClass* clazz, const std::string& name, DexType* type, uint64_t expected) {
    auto field_name = DexString::make_string(name);
    auto field = resolve_field(clazz->get_type(), field_name, type, FieldSearch::Static);
    ASSERT_NE(field, nullptr) << "Failed resolving field " << name
                              << " in class " << clazz->c_str();
    auto val = field->get_static_value();
    ASSERT_NE(val, nullptr) << "Failed getting static value for field " << field->c_str()
                            << " in class " << clazz->c_str();
    ASSERT_EQ(val->value(), expected) << "Incorrect value for field " << field->c_str()
                                      << " in class " << clazz->c_str();
  }
};

DexEncodedValue* make_ev(DexType* type, uint64_t val) {
  auto ev = DexEncodedValue::zero_for_type(type);
  ev->value(val);
  return ev;
}

// Create the named class with an empty clinit
DexClass* create_class(const std::string& name) {
  auto type = DexType::make_type(DexString::make_string(name));
  ClassCreator creator(type);
  creator.set_super(get_object_type());
  auto cls = creator.create();
  auto clinit_name = DexString::make_string("<clinit>");
  auto void_args = DexTypeList::make_type_list({});
  auto void_void = DexProto::make_proto(get_void_type(), void_args);
  auto clinit = DexMethod::make_method(type, clinit_name, void_void);
  clinit->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_CONSTRUCTOR, false);
  clinit->get_code()->set_registers_size(1);
  cls->add_method(clinit);
  return cls;
}

// Add a field that is initialized to a value
DexField* add_concrete_field(DexClass* cls, const std::string& name, DexType* type, uint64_t val) {
  auto container = cls->get_type();
  auto field_name = DexString::make_string(name);
  auto field = DexField::make_field(container, field_name, type);
  auto ev = make_ev(type, val);
  field->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL, ev);
  cls->add_field(field);
  return field;
}

// Add a field that is initialized to the value of parent
DexField* add_dependent_field(DexClass* cls, const std::string& name, DexField *parent) {
  // Create the field
  auto container = cls->get_type();
  auto field_name = DexString::make_string(name);
  auto field = DexField::make_field(container, field_name, parent->get_type());
  field->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
  cls->add_field(field);
  // Initialize it to the value of the parent
  auto parent_type = parent->get_type();
  assert(init_ops.count(parent_type->c_str()) != 0);
  auto ops = init_ops[parent_type->c_str()];
  auto clinit = cls->get_clinit();
  auto mt = clinit->get_code()->get_entries();
  auto sget = new IRFieldInstruction(ops.first, parent);
  sget->set_dest(0);
  mt->push_back(sget);
  auto sput = new IRFieldInstruction(ops.second, field);
  sput->set_src(0, 0);
  mt->push_back(sput);
  return field;
}

struct SimplePropagateTestCase {
  std::string type_name;
  DexType* type;
  uint64_t value;
};

// Check that we can do a simple, single level propagation. As source, this
// would look like:
//
//   class Parent {
//     public static final int CONST = 1;
//   }
//
//   class Child {
//     public static final int CONST = Parent.CONST;
//   }
TEST_F(ConstPropTest, simplePropagate) {
  SimplePropagateTestCase test_cases[] = {
    {"int", m_int_type, 12345},
    {"bool", m_bool_type, 1},
    {"byte", m_byte_type, 'b'},
    {"char", m_char_type, 'c'},
    {"short", m_short_type, 256}
  };
  for (auto test_case : test_cases) {
    auto parent = create_class("Lcom/redex/Parent_" + test_case.type_name + ";");
    auto parent_field = add_concrete_field(parent, "CONST", test_case.type, test_case.value);

    auto child = create_class("Lcom/redex/Child_" + test_case.type_name + ";");
    auto child_field = add_dependent_field(child, "CONST", parent_field);

    Scope classes = {parent, child};
    FinalInlinePass::propagate_constants(classes);

    expect_empty_clinit(child);
    expect_field_eq(child, "CONST", test_case.type, test_case.value);
  }
}

struct FieldDescriptor {
  std::string name;
  DexType* type;
  uint64_t value;
};

// Check that we can do a simple, single level propagation with multiple fields. As source,
// this would look like:
//
//   class Parent {
//     public static final int CONST_INT = 1111;
//     public static final bool CONST_BOOL = false;
//     public static final byte CONST_BYTE = 'b';
//     public static final char CONST_CHAR = 'c';
//     public static final short CONST_SHORT = 555;
//   }
//
//   class Child {
//     public static final int CONST_INT = Parent.CONST_INT;
//     public static final bool CONST_BOOL = Parent.CONST_BOOL;
//     public static final byte CONST_BYTE = Parent.CONST_BYTE;
//     public static final char CONST_CHAR = Parent.CONST_CHAR;
//     public static final short CONST_SHORT = Parent.CONST_SHORT;
//   }
TEST_F(ConstPropTest, simplePropagateMultiField) {
  FieldDescriptor field_descs[] = {
    {"CONST_INT", m_int_type, 1111},
    {"CONST_BOOL", m_bool_type, 0},
    {"CONST_BYTE", m_byte_type, 'b'},
    {"CONST_CHAR", m_char_type, 'c'},
    {"CONST_SHORT", m_short_type, 555}
  };
  auto parent = create_class("Lcom/redex/Parent;");
  auto child = create_class("Lcom/redex/Child;");
  for (auto fd : field_descs) {
    auto parent_field = add_concrete_field(parent, fd.name, fd.type, fd.value);
    add_dependent_field(child, fd.name, parent_field);
  }

  Scope classes = {parent, child};
  FinalInlinePass::propagate_constants(classes);

  expect_empty_clinit(child);
  for (auto fd : field_descs) {
    expect_field_eq(child, fd.name, fd.type, fd.value);
  }
}

// Check that we can propagate across multiple levels of dependencies. As source, this
// looks like:
//   class Parent {
//     public static final int CONST_INT = 1111;
//     public static final bool CONST_BOOL = false;
//     public static final byte CONST_BYTE = 'b';
//     public static final char CONST_CHAR = 'c';
//     public static final short CONST_SHORT = 555;
//   }
//
//   class Child {
//     public static final int CONST_INT = Parent.CONST_INT;
//     public static final bool CONST_BOOL = Parent.CONST_BOOL;
//     public static final byte CONST_BYTE = Parent.CONST_BYTE;
//     public static final char CONST_CHAR = Parent.CONST_CHAR;
//     public static final short CONST_SHORT = Parent.CONST_SHORT;
//   }
//
//   class GrandChild {
//     public static final int CONST_INT = Child.CONST_INT;
//     public static final bool CONST_BOOL = Child.CONST_BOOL;
//     public static final byte CONST_BYTE = Child.CONST_BYTE;
//     public static final char CONST_CHAR = Child.CONST_CHAR;
//     public static final short CONST_SHORT = Child.CONST_SHORT;
//   }
TEST_F(ConstPropTest, multiLevelPropagate) {
  FieldDescriptor field_descs[] = {
    {"CONST_INT", m_int_type, 1111},
    {"CONST_BOOL", m_bool_type, 0},
    {"CONST_BYTE", m_byte_type, 'b'},
    {"CONST_CHAR", m_char_type, 'c'},
    {"CONST_SHORT", m_short_type, 555}
  };
  auto parent = create_class("Lcom/redex/Parent;");
  auto child = create_class("Lcom/redex/Child;");
  auto grandchild = create_class("Lcom/redex/GrandChild;");
  for (auto fd : field_descs) {
    auto parent_field = add_concrete_field(parent, fd.name, fd.type, fd.value);
    auto child_field = add_dependent_field(child, fd.name, parent_field);
    add_dependent_field(grandchild, fd.name, child_field);
  }

  Scope classes = {parent, child, grandchild};
  FinalInlinePass::propagate_constants(classes);

  std::vector<DexClass*> descendants = {child, grandchild};
  for (auto clazz : descendants) {
    expect_empty_clinit(clazz);
    for (auto fd : field_descs) {
      expect_field_eq(clazz, fd.name, fd.type, fd.value);
    }
  }
}

// Check that we can propagate across multiple levels of dependencies where there
// are siblings at each level. In source, this looks like:
//
//   class Parent1 {
//     public static final int CONST_INT = 1111;
//     public static final char CONST_CHAR = 'a';
//   }
//
//   class Parent2 {
//     public static final int CONST_INT = 2222;
//     public static final char CONST_CHAR = 'b';
//   }
//
//   class Child1 {
//     public static final int CONST_INT = Parent1.CONST_INT;
//     public static final char CONST_CHAR = Parent2.CONST_CHAR;
//     public static final bool CONST_BOOL = true;
//   }
//
//   class Child2 {
//     public static final int CONST_INT = Parent2.CONST_INT;
//     public static final char CONST_CHAR = Parent1.CONST_CHAR;
//     public static final bool CONST_BOOL = false;
//   }
//
//   class GrandChild1 {
//     public static final int CONST_INT = Child1.CONST_INT;
//     public static final char CONST_CHAR = Child1.CONST_CHAR;
//     public static final bool CONST_BOOL = Child1.CONST_BOOL;
//   }
//
//   class GrandChild2 {
//     public static final int CONST_INT = Child2.CONST_INT;
//     public static final int CONST_CHAR = Child2.CONST_CHAR;
//     public static final bool CONST_BOOL = Child2.CONST_BOOL;
//   }
TEST_F(ConstPropTest, multiLevelWithSiblings) {
  auto parent1 = create_class("Lcom/redex/Parent1;");
  auto parent1_int = add_concrete_field(parent1, "CONST_INT", m_int_type, 1111);
  auto parent1_char = add_concrete_field(parent1, "CONST_CHAR", m_char_type, 'a');

  auto parent2 = create_class("Lcom/redex/Parent2;");
  auto parent2_int = add_concrete_field(parent2, "CONST_INT", m_int_type, 2222);
  auto parent2_char = add_concrete_field(parent2, "CONST_CHAR", m_char_type, 'b');

  auto child1 = create_class("Lcom/redex/Child1;");
  auto child1_int = add_dependent_field(child1, "CONST_INT", parent1_int);
  auto child1_char = add_dependent_field(child1, "CONST_CHAR", parent2_char);
  auto child1_bool = add_concrete_field(child1, "CONST_BOOL", m_bool_type, 1);

  auto child2 = create_class("Lcom/redex/Child2;");
  auto child2_int = add_dependent_field(child2, "CONST_INT", parent2_int);
  auto child2_char = add_dependent_field(child2, "CONST_CHAR", parent1_char);
  auto child2_bool = add_concrete_field(child2, "CONST_BOOL", m_bool_type, 0);

  auto grandchild1 = create_class("Lcom/redex/GrandChild1;");
  add_dependent_field(grandchild1, "CONST_INT", child1_int);
  add_dependent_field(grandchild1, "CONST_CHAR", child1_char);
  add_dependent_field(grandchild1, "CONST_BOOL", child1_bool);

  auto grandchild2 = create_class("Lcom/redex/GrandChild2;");
  add_dependent_field(grandchild2, "CONST_INT", child2_int);
  add_dependent_field(grandchild2, "CONST_CHAR", child2_char);
  add_dependent_field(grandchild2, "CONST_BOOL", child2_bool);

  Scope classes = {parent1, parent2, child1, child2, grandchild1, grandchild2};
  FinalInlinePass::propagate_constants(classes);

  Scope descendents = {child1, child2, grandchild1, grandchild2};
  for (auto clazz : descendents) {
    expect_empty_clinit(clazz);
  }

  expect_field_eq(child1, "CONST_INT", m_int_type, 1111);
  expect_field_eq(child1, "CONST_CHAR", m_char_type, 'b');
  expect_field_eq(child2, "CONST_INT", m_int_type, 2222);
  expect_field_eq(child2, "CONST_CHAR", m_char_type, 'a');
  expect_field_eq(grandchild1, "CONST_INT", m_int_type, 1111);
  expect_field_eq(grandchild1, "CONST_CHAR", m_char_type, 'b');
  expect_field_eq(grandchild1, "CONST_BOOL", m_bool_type, 1);
  expect_field_eq(grandchild2, "CONST_INT", m_int_type, 2222);
  expect_field_eq(grandchild2, "CONST_CHAR", m_char_type, 'a');
  expect_field_eq(grandchild2, "CONST_BOOL", m_bool_type, 0);
}
