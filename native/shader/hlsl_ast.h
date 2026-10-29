#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace raven::shader {

enum class Scalar : uint8_t { boolean, integer, unsigned_integer, floating, half, doubled, none };

enum class ObjectKind : uint8_t {
	none, sampler, sampler_comparison, texture1d, texture1d_array, texture2d, texture2d_array, texture2dms, texture2dms_array, texture3d,
	texture_cube, texture_cube_array, buffer, structured_buffer, byte_address_buffer, rw_texture1d, rw_texture1d_array, rw_texture2d,
	rw_texture2d_array, rw_texture3d, rw_buffer, rw_structured_buffer, rw_byte_address_buffer
};

struct StructType;

// A value's type: void, a scalar, a vector of one to four components, a matrix of rows by columns, a structure or a resource object; any
// of them may be an array.
struct Type {
	enum class Shape : uint8_t { void_type, scalar, vector, matrix, structure, object } shape = Shape::void_type;
	Scalar scalar = Scalar::none;
	uint8_t rows = 1, columns = 1;
	// The element type of a texture or buffer (float4 by default).
	std::shared_ptr<Type> element;
	ObjectKind object = ObjectKind::none;
	std::shared_ptr<StructType> structure;
	// Outermost first; zero means the size comes from the initializer.
	std::vector<uint32_t> array;
	bool row_major = false;

	static Type make(Shape shape, Scalar kind, uint8_t rows, uint8_t columns)
	{
		Type type;
		type.shape = shape;
		type.scalar = kind;
		type.rows = rows;
		type.columns = columns;
		return type;
	}
	static Type make_scalar(Scalar kind) { return make(Shape::scalar, kind, 1, 1); }
	static Type make_vector(Scalar kind, uint8_t count) { return count == 1 ? make_scalar(kind) : make(Shape::vector, kind, 1, count); }
	static Type make_matrix(Scalar kind, uint8_t rows, uint8_t columns) { return make(Shape::matrix, kind, rows, columns); }
	bool numeric() const { return array.empty() && (shape == Shape::scalar || shape == Shape::vector || shape == Shape::matrix); }
	uint32_t components() const { return shape == Shape::scalar ? 1 : shape == Shape::vector ? columns : shape == Shape::matrix ? rows * columns : 0; }
	bool operator==(const Type &other) const;
	std::string name() const;
};

struct Member {
	std::string name, semantic;
	Type type;
	// Interpolation modifiers of a stage input or output: linear, centroid, nointerpolation, noperspective, sample.
	std::vector<std::string> modifiers;
};

struct StructType {
	std::string name;
	std::vector<Member> members;
	// The structure this one derives from, whose members come first and whose methods it inherits.
	std::shared_ptr<StructType> base;
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
	enum class Kind : uint8_t {
		literal, name, unary, binary, assign, ternary, call, member, index, cast, construct, initializer_list, postfix
	} kind;
	uint32_t line = 0;
	// The operator of a unary, binary, assignment or postfix expression; the name of a variable, function or member.
	std::string text;
	double number = 0;
	Type literal_type;
	Type cast_type;
	std::vector<ExprPtr> operands;
	// Filled in by the checker.
	Type type;
};

struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Variable {
	std::string name, semantic;
	Type type;
	ExprPtr initializer;
	// register(t0), register(s1), register(b2), register(u3), packoffset(c1.y): the class letter and number; -1 when unbound.
	char register_class = 0;
	int register_index = -1;
	int pack_offset = -1;
	bool is_static = false, is_const = false, is_uniform = false, is_groupshared = false;
	std::vector<std::string> modifiers;
	uint32_t line = 0;
};

struct Stmt {
	enum class Kind : uint8_t { block, declaration, expression, if_statement, for_statement, while_statement, do_statement, return_statement,
		break_statement, continue_statement, discard_statement, switch_statement, empty } kind;
	uint32_t line = 0;
	std::vector<StmtPtr> body;
	std::vector<Variable> variables;
	// The condition of if, while, do and for, the value of return and expression statements; for: init in body[0], step in step.
	ExprPtr value, step;
	StmtPtr then_branch, else_branch;
	// Attributes such as [unroll], [loop], [branch], [flatten].
	std::vector<std::string> attributes;
	// switch: the case labels; a default label has no value.
	std::vector<std::pair<std::optional<int64_t>, std::vector<StmtPtr>>> cases;
};

struct Parameter {
	std::string name, semantic;
	Type type;
	// in, out, inout, uniform; plus the interpolation and primitive modifiers.
	bool is_in = true, is_out = false, is_uniform = false;
	std::vector<std::string> modifiers;
	ExprPtr default_value;
};

struct Function {
	std::string name, semantic;
	Type result;
	std::vector<Parameter> parameters;
	std::unique_ptr<Stmt> body;
	// [numthreads(x, y, z)], [maxvertexcount(n)], [domain("tri")] and the rest, with their arguments as written.
	std::vector<std::pair<std::string, std::vector<std::string>>> attributes;
	// A method: the structure it belongs to, whose members its body names directly.
	const StructType *owner = nullptr;
	uint32_t line = 0;
};

struct ConstantBuffer {
	std::string name;
	bool texture_buffer = false;
	int register_index = -1;
	std::vector<Variable> members;
	uint32_t line = 0;
};

struct Program {
	std::vector<std::shared_ptr<StructType>> structures;
	std::vector<Variable> globals;
	std::vector<ConstantBuffer> buffers;
	std::vector<Function> functions;
	// typedef names to their types.
	std::vector<std::pair<std::string, Type>> aliases;
};

}
