#include "hlsl_parser.h"

#include <cstring>
#include <map>
#include <stdexcept>

namespace raven::shader {

namespace {

// A new expression or statement node of a kind, every other field at its default.
template<typename Node>
std::unique_ptr<Node> make_node(typename Node::Kind kind)
{
	auto made = std::make_unique<Node>();
	made->kind = kind;
	return made;
}

struct ParseError : std::runtime_error {
	uint32_t line;
	ParseError(uint32_t line, const std::string &message) : std::runtime_error(message), line(line) {}
};

const std::map<std::string, Scalar> &scalar_names()
{
	static const std::map<std::string, Scalar> names{{"bool", Scalar::boolean}, {"int", Scalar::integer}, {"uint", Scalar::unsigned_integer},
		{"dword", Scalar::unsigned_integer}, {"float", Scalar::floating}, {"half", Scalar::half}, {"double", Scalar::doubled},
		{"min16float", Scalar::half}, {"min10float", Scalar::half}, {"min16int", Scalar::integer}, {"min12int", Scalar::integer},
		{"min16uint", Scalar::unsigned_integer}};
	return names;
}

const std::map<std::string, ObjectKind> &object_names()
{
	static const std::map<std::string, ObjectKind> names{{"SamplerState", ObjectKind::sampler}, {"sampler", ObjectKind::sampler},
		{"sampler2D", ObjectKind::sampler}, {"SamplerComparisonState", ObjectKind::sampler_comparison}, {"Texture1D", ObjectKind::texture1d},
		{"Texture1DArray", ObjectKind::texture1d_array}, {"Texture2D", ObjectKind::texture2d}, {"Texture2DArray", ObjectKind::texture2d_array},
		{"Texture2DMS", ObjectKind::texture2dms}, {"Texture2DMSArray", ObjectKind::texture2dms_array}, {"Texture3D", ObjectKind::texture3d},
		{"TextureCube", ObjectKind::texture_cube}, {"TextureCubeArray", ObjectKind::texture_cube_array}, {"Buffer", ObjectKind::buffer},
		{"StructuredBuffer", ObjectKind::structured_buffer}, {"ByteAddressBuffer", ObjectKind::byte_address_buffer},
		{"RWTexture1D", ObjectKind::rw_texture1d}, {"RWTexture1DArray", ObjectKind::rw_texture1d_array}, {"RWTexture2D", ObjectKind::rw_texture2d},
		{"RWTexture2DArray", ObjectKind::rw_texture2d_array}, {"RWTexture3D", ObjectKind::rw_texture3d}, {"RWBuffer", ObjectKind::rw_buffer},
		{"RWStructuredBuffer", ObjectKind::rw_structured_buffer}, {"RWByteAddressBuffer", ObjectKind::rw_byte_address_buffer}};
	return names;
}

// "float3x4", "int2": the scalar and the dimensions written after it.
std::optional<Type> numeric_name(const std::string &text)
{
	for (const auto &[name, kind] : scalar_names()) {
		if (!text.starts_with(name))
			continue;
		const auto rest = text.substr(name.size());
		if (rest.empty())
			return Type::make_scalar(kind);
		if (rest.size() == 1 && rest[0] >= '1' && rest[0] <= '4')
			return Type::make_vector(kind, uint8_t(rest[0] - '0'));
		if (rest.size() == 3 && rest[1] == 'x' && rest[0] >= '1' && rest[0] <= '4' && rest[2] >= '1' && rest[2] <= '4')
			return Type::make_matrix(kind, uint8_t(rest[0] - '0'), uint8_t(rest[2] - '0'));
	}
	return std::nullopt;
}

class Parser {
public:
	Parser(const std::vector<Token> &tokens, Program &program) : tokens(tokens), program(program) {}

	void run()
	{
		while (peek().kind != TokenKind::end)
			top_level();
	}

private:
	const std::vector<Token> &tokens;
	Program &program;
	size_t at = 0;

	const Token &peek(size_t ahead = 0) const { return tokens[std::min(at + ahead, tokens.size() - 1)]; }
	const Token &next() { return tokens[at < tokens.size() - 1 ? at++ : at]; }
	bool is(std::string_view text, size_t ahead = 0) const { return peek(ahead).kind != TokenKind::string && peek(ahead).text == text; }
	bool accept(std::string_view text)
	{
		if (!is(text))
			return false;
		++at;
		return true;
	}
	[[noreturn]] void fail(const std::string &message) const { throw ParseError(peek().line, message); }
	void expect(std::string_view text)
	{
		if (!accept(text))
			fail("expected '" + std::string(text) + "' before '" + peek().text + "'");
	}
	std::string identifier()
	{
		if (peek().kind != TokenKind::identifier)
			fail("expected a name before '" + peek().text + "'");
		return next().text;
	}

	std::shared_ptr<StructType> find_struct(const std::string &name) const
	{
		for (const auto &structure : program.structures)
			if (structure->name == name)
				return structure;
		return nullptr;
	}

	bool is_type_start(size_t ahead = 0) const
	{
		const auto &token = peek(ahead);
		if (token.kind != TokenKind::identifier)
			return false;
		static const char *const prefixes[] = {"const", "static", "uniform", "row_major", "column_major", "volatile", "extern", "groupshared",
			"precise", "inline", "linear", "centroid", "nointerpolation", "noperspective", "sample", "in", "out", "inout", "struct", "class", "vector",
			"matrix", "void", "snorm", "unorm", "globallycoherent", "shared", "unsigned", "signed"};
		for (const auto prefix : prefixes)
			if (token.text == prefix)
				return true;
		if (numeric_name(token.text) || object_names().contains(token.text) || find_struct(token.text))
			return true;
		for (const auto &[name, type] : program.aliases)
			if (name == token.text)
				return true;
		return false;
	}

	// A template argument list such as <float4> or <float, 3>.
	std::vector<std::string> template_arguments()
	{
		std::vector<std::string> arguments;
		if (!accept("<"))
			return arguments;
		std::string current;
		int depth = 0;
		while (peek().kind != TokenKind::end) {
			if (is(">") && !depth)
				break;
			if (is(">>") && depth == 1) {
				// Texture2D<vector<float, 4>> closes two lists with one token.
				current += ">";
				++at;
				arguments.push_back(current);
				return arguments;
			}
			if (is(",") && !depth) {
				arguments.push_back(current);
				current.clear();
				++at;
				continue;
			}
			depth += is("<") ? 1 : is(">") ? -1 : 0;
			current += (current.empty() ? "" : " ") + next().text;
		}
		expect(">");
		arguments.push_back(current);
		return arguments;
	}

	Type type_from_text(const std::string &text)
	{
		if (auto numeric = numeric_name(text))
			return *numeric;
		if (const auto structure = find_struct(text)) {
			Type type;
			type.shape = Type::Shape::structure;
			type.structure = structure;
			return type;
		}
		std::vector<Token> inner;
		std::string errors;
		inner = preprocess(text, "", {}, {}).tokens;
		Parser nested(inner, program);
		return nested.type();
	}

	// A type with its leading modifiers; the modifiers are returned through `modifiers`.
	Type type(std::vector<std::string> *modifiers = nullptr)
	{
		bool row_major = false;
		for (;;) {
			static const char *const words[] = {"const", "static", "uniform", "row_major", "column_major", "volatile", "extern", "groupshared",
				"precise", "inline", "linear", "centroid", "nointerpolation", "noperspective", "sample", "in", "out", "inout", "snorm", "unorm",
				"globallycoherent", "shared"};
			bool found = false;
			for (const auto word : words)
				if (is(word)) {
					if (modifiers)
						modifiers->push_back(word);
					row_major |= std::strcmp(word, "row_major") == 0;
					++at;
					found = true;
					break;
				}
			if (!found)
				break;
		}
		Type result;
		if (accept("void"))
			return result;
		if (is("struct") || is("class")) {
			result.shape = Type::Shape::structure;
			result.structure = structure_definition();
			return result;
		}
		// "unsigned int" is uint, "signed int" int.
		if (is("unsigned") || is("signed")) {
			const bool is_unsigned = next().text == "unsigned";
			accept("int");
			result = Type::make_scalar(is_unsigned ? Scalar::unsigned_integer : Scalar::integer);
			result.row_major = row_major;
			return result;
		}
		const auto name = identifier();
		if (auto numeric = numeric_name(name))
			result = *numeric;
		else if (name == "vector" || name == "matrix") {
			const auto arguments = template_arguments();
			if (arguments.empty())
				result = name == "vector" ? Type::make_vector(Scalar::floating, 4) : Type::make_matrix(Scalar::floating, 4, 4);
			else {
				const auto element = type_from_text(arguments[0]);
				if (name == "vector")
					result = Type::make_vector(element.scalar, uint8_t(arguments.size() > 1 ? std::stoi(arguments[1]) : 4));
				else
					result = Type::make_matrix(element.scalar, uint8_t(arguments.size() > 1 ? std::stoi(arguments[1]) : 4),
						uint8_t(arguments.size() > 2 ? std::stoi(arguments[2]) : 4));
			}
		} else if (const auto object = object_names().find(name); object != object_names().end()) {
			result.shape = Type::Shape::object;
			result.object = object->second;
			const auto arguments = template_arguments();
			const bool sampler = object->second == ObjectKind::sampler || object->second == ObjectKind::sampler_comparison;
			if (!sampler && object->second != ObjectKind::byte_address_buffer && object->second != ObjectKind::rw_byte_address_buffer)
				result.element = std::make_shared<Type>(arguments.empty() ? Type::make_vector(Scalar::floating, 4) : type_from_text(arguments[0]));
		} else if (const auto structure = find_struct(name)) {
			result.shape = Type::Shape::structure;
			result.structure = structure;
		} else {
			bool aliased = false;
			for (const auto &[alias, aliased_type] : program.aliases)
				if (alias == name) {
					result = aliased_type;
					aliased = true;
				}
			if (!aliased)
				throw ParseError(peek().line, "unknown type '" + name + "'");
		}
		result.row_major = row_major;
		return result;
	}

	void skip_braces()
	{
		int depth = 0;
		do {
			depth += is("{") ? 1 : is("}") ? -1 : 0;
			++at;
		} while (depth && peek().kind != TokenKind::end);
	}

	void array_suffix(Type &type)
	{
		while (accept("[")) {
			uint32_t size = 0;
			if (!is("]")) {
				auto count = expression();
				size = uint32_t(constant(*count));
			}
			expect("]");
			type.array.push_back(size);
		}
	}

	// The value of a constant integer expression in an array size or case label.
	int64_t constant(const Expr &expr)
	{
		switch (expr.kind) {
		case Expr::Kind::literal:
			return int64_t(expr.number);
		case Expr::Kind::unary:
			if (expr.text == "-")
				return -constant(*expr.operands[0]);
			if (expr.text == "+")
				return constant(*expr.operands[0]);
			if (expr.text == "~")
				return ~constant(*expr.operands[0]);
			break;
		case Expr::Kind::binary: {
			const auto left = constant(*expr.operands[0]), right = constant(*expr.operands[1]);
			if (expr.text == "+") return left + right;
			if (expr.text == "-") return left - right;
			if (expr.text == "*") return left * right;
			if (expr.text == "/" && right) return left / right;
			if (expr.text == "%" && right) return left % right;
			if (expr.text == "<<") return left << right;
			if (expr.text == ">>") return left >> right;
			if (expr.text == "&") return left & right;
			if (expr.text == "|") return left | right;
			break;
		}
		case Expr::Kind::name:
			for (const auto &global : program.globals)
				if (global.name == expr.text && global.is_static && global.is_const && global.initializer)
					return constant(*global.initializer);
			break;
		case Expr::Kind::cast:
			return constant(*expr.operands[0]);
		default:
			break;
		}
		throw ParseError(expr.line, "expected a constant integer expression");
	}

	// The register and semantic annotations after a declaration: ": SEMANTIC", ": register(t0)", ": packoffset(c0.x)".
	void annotations(std::string &semantic, char *register_class, int *register_index, int *pack_offset)
	{
		while (accept(":")) {
			if (accept("register")) {
				expect("(");
				const auto text = identifier();
				// register(space0, t1) and register(t1, space0) both name the slot.
				std::string slot = text;
				if (accept(",")) {
					const auto other = identifier();
					if (slot.starts_with("space"))
						slot = other;
				}
				expect(")");
				if (register_class && register_index && slot.size() > 1) {
					*register_class = char(std::tolower(static_cast<unsigned char>(slot[0])));
					*register_index = std::stoi(slot.substr(1));
				}
			} else if (accept("packoffset")) {
				expect("(");
				const auto text = identifier();
				int component = 0;
				if (accept(".")) {
					const auto letter = identifier();
					component = std::string("xyzw").find(letter[0]) == std::string::npos ? 0 : int(std::string("xyzw").find(letter[0]));
				}
				expect(")");
				if (pack_offset && text.size() > 1)
					*pack_offset = std::stoi(text.substr(1)) * 4 + component;
			} else
				semantic = identifier();
		}
	}

	std::shared_ptr<StructType> structure_definition()
	{
		if (!accept("class"))
			expect("struct");
		std::string name = peek().kind == TokenKind::identifier ? next().text : "";
		std::shared_ptr<StructType> base;
		if (accept(":")) {
			const auto base_name = identifier();
			base = find_struct(base_name);
			if (!base)
				fail("unknown structure '" + base_name + "'");
		}
		if (!is("{")) {
			if (const auto existing = find_struct(name))
				return existing;
			fail("unknown structure '" + name + "'");
		}
		auto structure = std::make_shared<StructType>();
		structure->name = name;
		structure->base = base;
		if (base)
			structure->members = base->members;
		program.structures.push_back(structure);
		expect("{");
		while (!accept("}")) {
			Member member;
			member.type = type(&member.modifiers);
			if (peek().kind == TokenKind::identifier && is("(", 1)) {
				Function method;
				method.result = member.type;
				method.owner = structure.get();
				method.line = peek().line;
				method.name = identifier();
				function_rest(std::move(method));
				continue;
			}
			for (;;) {
				Member item = member;
				item.name = identifier();
				array_suffix(item.type);
				annotations(item.semantic, nullptr, nullptr, nullptr);
				structure->members.push_back(item);
				if (!accept(","))
					break;
			}
			expect(";");
		}
		return structure;
	}

	std::vector<std::pair<std::string, std::vector<std::string>>> attributes()
	{
		std::vector<std::pair<std::string, std::vector<std::string>>> list;
		while (is("[") && peek(1).kind == TokenKind::identifier && (is("]", 2) || is("(", 2))) {
			++at;
			auto name = identifier();
			std::vector<std::string> arguments;
			if (accept("(")) {
				while (!accept(")")) {
					const auto &token = next();
					if (token.text != ",")
						arguments.push_back(token.kind == TokenKind::string ? token.text.substr(1, token.text.size() - 2) : token.text);
				}
			}
			expect("]");
			list.emplace_back(std::move(name), std::move(arguments));
		}
		return list;
	}

	// The parameters, semantic and body of a function or method whose name was just read.
	void function_rest(Function function)
	{
		expect("(");
		while (!accept(")")) {
			Parameter parameter;
			std::vector<std::string> parameter_modifiers;
			// Geometry shader inputs and outputs name their primitive or stream before the type.
			static const char *const leading[] = {"in", "out", "inout", "uniform", "point", "line", "triangle", "lineadj", "triangleadj"};
			for (bool found = true; found;) {
				found = false;
				for (const auto word : leading)
					if (is(word) && peek(1).kind == TokenKind::identifier) {
						parameter_modifiers.push_back(word);
						++at;
						found = true;
					}
			}
			// A domain shader's control points come as a const OutputPatch.
			while (is("const") && (is("in", 1) || is("InputPatch", 1) || is("OutputPatch", 1)))
				++at;
			if (is("in") && (is("InputPatch", 1) || is("OutputPatch", 1)))
				++at;
			if (is("TriangleStream") || is("LineStream") || is("PointStream") || is("InputPatch") || is("OutputPatch")) {
				parameter_modifiers.push_back(next().text);
				auto arguments = template_arguments();
				parameter.type = arguments.empty() ? Type{} : type_from_text(arguments[0]);
				if (arguments.size() > 1)
					parameter.type.array.push_back(uint32_t(std::stoul(arguments[1])));
			} else
				parameter.type = type(&parameter_modifiers);
			for (const auto &modifier : parameter_modifiers) {
				if (modifier == "out") {
					parameter.is_in = false;
					parameter.is_out = true;
				} else if (modifier == "inout")
					parameter.is_out = true;
				else if (modifier == "uniform")
					parameter.is_uniform = true;
			}
			parameter.modifiers = std::move(parameter_modifiers);
			parameter.name = identifier();
			array_suffix(parameter.type);
			annotations(parameter.semantic, nullptr, nullptr, nullptr);
			if (accept("="))
				parameter.default_value = assignment();
			function.parameters.push_back(std::move(parameter));
			if (!is(")"))
				expect(",");
		}
		annotations(function.semantic, nullptr, nullptr, nullptr);
		if (accept(";")) {
			program.functions.push_back(std::move(function));
			return;
		}
		function.body = block();
		// A later definition replaces an earlier prototype of the same signature.
		for (auto &existing : program.functions)
			if (existing.name == function.name && existing.owner == function.owner && !existing.body &&
				existing.parameters.size() == function.parameters.size()) {
				existing = std::move(function);
				return;
			}
		program.functions.push_back(std::move(function));
	}

	void top_level()
	{
		if (accept(";"))
			return;
		auto attribute_list = attributes();
		if (is("typedef")) {
			++at;
			auto aliased = type();
			for (;;) {
				auto name = identifier();
				auto item = aliased;
				array_suffix(item);
				program.aliases.emplace_back(std::move(name), item);
				if (!accept(","))
					break;
			}
			expect(";");
			return;
		}
		// What the effect framework adds to a shader source, techniques and render state objects, means nothing to a shader target.
		if (is("technique") || is("technique10") || is("technique11") || is("fxgroup")) {
			while (!is("{") && peek().kind != TokenKind::end)
				++at;
			skip_braces();
			accept(";");
			return;
		}
		if (is("BlendState") || is("DepthStencilState") || is("RasterizerState")) {
			while (!is(";") && peek().kind != TokenKind::end) {
				if (is("{"))
					skip_braces();
				else
					++at;
			}
			expect(";");
			return;
		}
		if (is("cbuffer") || is("tbuffer")) {
			ConstantBuffer buffer;
			buffer.texture_buffer = next().text == "tbuffer";
			buffer.line = peek().line;
			buffer.name = identifier();
			std::string semantic;
			char register_class = 0;
			annotations(semantic, &register_class, &buffer.register_index, nullptr);
			expect("{");
			while (!accept("}")) {
				auto members = declaration_list();
				for (auto &member : members)
					buffer.members.push_back(std::move(member));
			}
			accept(";");
			program.buffers.push_back(std::move(buffer));
			return;
		}
		if ((is("struct") || is("class")) && (is("{", 2) || is(";", 2) || is(":", 2))) {
			structure_definition();
			expect(";");
			return;
		}
		// A function: a type, a name and a parenthesis.
		const size_t start = at;
		std::vector<std::string> modifiers;
		auto result = type(&modifiers);
		if (peek().kind == TokenKind::identifier && is("(", 1)) {
			Function function;
			function.attributes = std::move(attribute_list);
			function.result = result;
			function.line = peek().line;
			function.name = identifier();
			function_rest(std::move(function));
			return;
		}
		at = start;
		for (auto &variable : declaration_list())
			program.globals.push_back(std::move(variable));
	}

	// One declaration statement: a type and one or more names, each with its array size, annotations and initializer.
	std::vector<Variable> declaration_list()
	{
		std::vector<std::string> modifiers;
		const uint32_t line = peek().line;
		auto base = type(&modifiers);
		std::vector<Variable> list;
		for (;;) {
			Variable variable;
			variable.line = line;
			variable.type = base;
			variable.modifiers = modifiers;
			for (const auto &modifier : modifiers) {
				variable.is_static |= modifier == "static";
				variable.is_const |= modifier == "const";
				variable.is_uniform |= modifier == "uniform";
				variable.is_groupshared |= modifier == "groupshared";
			}
			variable.name = identifier();
			array_suffix(variable.type);
			annotations(variable.semantic, &variable.register_class, &variable.register_index, &variable.pack_offset);
			// Sampler state blocks of the effect framework are skipped.
			if (is("{") && variable.type.shape == Type::Shape::object)
				skip_braces();
			else if (accept("="))
				variable.initializer = is("{") ? initializer_list() : assignment();
			list.push_back(std::move(variable));
			if (!accept(","))
				break;
		}
		expect(";");
		return list;
	}

	ExprPtr initializer_list()
	{
		auto list = make_node<Expr>(Expr::Kind::initializer_list);
		list->line = peek().line;
		expect("{");
		while (!accept("}")) {
			list->operands.push_back(is("{") ? initializer_list() : assignment());
			if (!is("}"))
				expect(",");
		}
		return list;
	}

	std::unique_ptr<Stmt> block()
	{
		auto statement = make_node<Stmt>(Stmt::Kind::block);
		statement->line = peek().line;
		expect("{");
		while (!accept("}")) {
			if (peek().kind == TokenKind::end)
				fail("missing '}'");
			statement->body.push_back(this->statement());
		}
		return statement;
	}

	StmtPtr make(Stmt::Kind kind)
	{
		auto statement = make_node<Stmt>(kind);
		statement->line = peek().line;
		return statement;
	}

	StmtPtr statement()
	{
		std::vector<std::string> attribute_names;
		for (auto &[name, arguments] : attributes())
			attribute_names.push_back(name);
		if (is("{"))
			return block();
		if (accept(";"))
			return make(Stmt::Kind::empty);
		if (accept("if")) {
			auto statement = make(Stmt::Kind::if_statement);
			statement->attributes = std::move(attribute_names);
			expect("(");
			statement->value = expression();
			expect(")");
			statement->then_branch = this->statement();
			if (accept("else"))
				statement->else_branch = this->statement();
			return statement;
		}
		if (accept("for")) {
			auto statement = make(Stmt::Kind::for_statement);
			statement->attributes = std::move(attribute_names);
			expect("(");
			if (accept(";"))
				statement->body.push_back(make(Stmt::Kind::empty));
			else if (is_type_start()) {
				auto declaration = make(Stmt::Kind::declaration);
				declaration->variables = declaration_list();
				statement->body.push_back(std::move(declaration));
			} else {
				auto init = make(Stmt::Kind::expression);
				init->value = expression();
				expect(";");
				statement->body.push_back(std::move(init));
			}
			if (!is(";"))
				statement->value = expression();
			expect(";");
			if (!is(")"))
				statement->step = expression();
			expect(")");
			statement->then_branch = this->statement();
			return statement;
		}
		if (accept("while")) {
			auto statement = make(Stmt::Kind::while_statement);
			statement->attributes = std::move(attribute_names);
			expect("(");
			statement->value = expression();
			expect(")");
			statement->then_branch = this->statement();
			return statement;
		}
		if (accept("do")) {
			auto statement = make(Stmt::Kind::do_statement);
			statement->then_branch = this->statement();
			expect("while");
			expect("(");
			statement->value = expression();
			expect(")");
			expect(";");
			return statement;
		}
		if (accept("switch")) {
			auto statement = make(Stmt::Kind::switch_statement);
			expect("(");
			statement->value = expression();
			expect(")");
			expect("{");
			while (!accept("}")) {
				std::optional<int64_t> label;
				if (accept("default"))
					expect(":");
				else {
					expect("case");
					label = constant(*expression());
					expect(":");
				}
				std::vector<StmtPtr> body;
				while (!is("case") && !is("default") && !is("}"))
					body.push_back(this->statement());
				statement->cases.emplace_back(label, std::move(body));
			}
			return statement;
		}
		if (accept("return")) {
			auto statement = make(Stmt::Kind::return_statement);
			if (!is(";"))
				statement->value = expression();
			expect(";");
			return statement;
		}
		if (accept("break")) {
			expect(";");
			return make(Stmt::Kind::break_statement);
		}
		if (accept("continue")) {
			expect(";");
			return make(Stmt::Kind::continue_statement);
		}
		if (accept("discard")) {
			expect(";");
			return make(Stmt::Kind::discard_statement);
		}
		if (is_type_start() && !(is("(", 1))) {
			auto statement = make(Stmt::Kind::declaration);
			statement->variables = declaration_list();
			return statement;
		}
		auto statement = make(Stmt::Kind::expression);
		statement->value = expression();
		expect(";");
		return statement;
	}

	ExprPtr node(Expr::Kind kind, std::string text, uint32_t line)
	{
		auto expr = make_node<Expr>(kind);
		expr->text = std::move(text);
		expr->line = line;
		return expr;
	}

	ExprPtr expression()
	{
		auto left = assignment();
		while (is(",")) {
			const auto line = next().line;
			auto comma = node(Expr::Kind::binary, ",", line);
			comma->operands.push_back(std::move(left));
			comma->operands.push_back(assignment());
			left = std::move(comma);
		}
		return left;
	}

	ExprPtr assignment()
	{
		auto left = conditional();
		static const char *const operators[] = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
		for (const auto op : operators)
			if (is(op)) {
				const auto line = next().line;
				auto assign = node(Expr::Kind::assign, op, line);
				assign->operands.push_back(std::move(left));
				assign->operands.push_back(assignment());
				return assign;
			}
		return left;
	}

	ExprPtr conditional()
	{
		auto condition = binary(0);
		if (!is("?"))
			return condition;
		const auto line = next().line;
		auto choice = node(Expr::Kind::ternary, "?", line);
		choice->operands.push_back(std::move(condition));
		choice->operands.push_back(assignment());
		expect(":");
		choice->operands.push_back(assignment());
		return choice;
	}

	static int precedence(const Token &token)
	{
		if (token.kind != TokenKind::punct)
			return -1;
		static const std::map<std::string, int> table{{"||", 0}, {"&&", 1}, {"|", 2}, {"^", 3}, {"&", 4}, {"==", 5}, {"!=", 5}, {"<", 6},
			{">", 6}, {"<=", 6}, {">=", 6}, {"<<", 7}, {">>", 7}, {"+", 8}, {"-", 8}, {"*", 9}, {"/", 9}, {"%", 9}};
		const auto found = table.find(token.text);
		return found == table.end() ? -1 : found->second;
	}

	ExprPtr binary(int minimum)
	{
		auto left = unary();
		for (;;) {
			const int level = precedence(peek());
			if (level < minimum)
				return left;
			const auto &token = next();
			auto combined = node(Expr::Kind::binary, token.text, token.line);
			combined->operands.push_back(std::move(left));
			combined->operands.push_back(binary(level + 1));
			left = std::move(combined);
		}
	}

	ExprPtr unary()
	{
		if (is("-") || is("+") || is("!") || is("~") || is("++") || is("--")) {
			const auto &token = next();
			auto expr = node(Expr::Kind::unary, token.text, token.line);
			expr->operands.push_back(unary());
			return expr;
		}
		// A cast: a parenthesized type followed by an operand.
		if (is("(") && is_type_start(1)) {
			const size_t start = at;
			++at;
			try {
				auto target = type();
				array_suffix(target);
				if (accept(")")) {
					auto expr = node(Expr::Kind::cast, "", peek().line);
					expr->cast_type = target;
					expr->operands.push_back(unary());
					return expr;
				}
			} catch (const ParseError &) {
			}
			at = start;
		}
		return postfix(primary());
	}

	ExprPtr postfix(ExprPtr expr)
	{
		for (;;) {
			if (is(".")) {
				const auto line = next().line;
				auto member = node(Expr::Kind::member, identifier(), line);
				member->operands.push_back(std::move(expr));
				if (is("(")) {
					// A method call: the object is the first operand.
					auto call = node(Expr::Kind::call, member->text, line);
					call->operands.push_back(std::move(member->operands[0]));
					arguments(*call);
					expr = std::move(call);
					expr->number = 1;
				} else
					expr = std::move(member);
			} else if (is("[")) {
				const auto line = next().line;
				auto index = node(Expr::Kind::index, "[]", line);
				index->operands.push_back(std::move(expr));
				index->operands.push_back(expression());
				expect("]");
				expr = std::move(index);
			} else if (is("++") || is("--")) {
				const auto &token = next();
				auto post = node(Expr::Kind::postfix, token.text, token.line);
				post->operands.push_back(std::move(expr));
				expr = std::move(post);
			} else
				return expr;
		}
	}

	void arguments(Expr &call)
	{
		expect("(");
		while (!accept(")")) {
			call.operands.push_back(assignment());
			if (!is(")"))
				expect(",");
		}
	}

	ExprPtr primary()
	{
		const auto &token = peek();
		if (token.kind == TokenKind::integer || token.kind == TokenKind::floating) {
			++at;
			auto literal = node(Expr::Kind::literal, token.text, token.line);
			literal->number = token.number;
			literal->literal_type = Type::make_scalar(token.kind == TokenKind::floating ? Scalar::floating :
				token.is_unsigned ? Scalar::unsigned_integer : Scalar::integer);
			return literal;
		}
		if (token.text == "true" || token.text == "false") {
			++at;
			auto literal = node(Expr::Kind::literal, token.text, token.line);
			literal->number = token.text == "true";
			literal->literal_type = Type::make_scalar(Scalar::boolean);
			return literal;
		}
		if (accept("(")) {
			auto inner = expression();
			expect(")");
			return inner;
		}
		if (token.kind != TokenKind::identifier)
			fail("unexpected '" + token.text + "'");
		// A constructor: float4(...), int2x2(...), vector<float, 3>(...).
		if (numeric_name(token.text) || token.text == "vector" || token.text == "matrix") {
			const auto line = token.line;
			auto constructed = type();
			if (is("(")) {
				auto expr = node(Expr::Kind::construct, "", line);
				expr->cast_type = constructed;
				arguments(*expr);
				return expr;
			}
			fail("expected '(' after a type name");
		}
		++at;
		if (is("(")) {
			auto call = node(Expr::Kind::call, token.text, token.line);
			arguments(*call);
			return call;
		}
		return node(Expr::Kind::name, token.text, token.line);
	}
};

}

bool Type::operator==(const Type &other) const
{
	if (shape != other.shape || scalar != other.scalar || rows != other.rows || columns != other.columns || object != other.object ||
		structure != other.structure || array != other.array)
		return false;
	if (bool(element) != bool(other.element))
		return false;
	return !element || *element == *other.element;
}

std::string Type::name() const
{
	static const char *const scalars[] = {"bool", "int", "uint", "float", "half", "double", "none"};
	std::string text;
	switch (shape) {
	case Shape::void_type:
		text = "void";
		break;
	case Shape::scalar:
		text = scalars[int(scalar)];
		break;
	case Shape::vector:
		text = std::string(scalars[int(scalar)]) + std::to_string(columns);
		break;
	case Shape::matrix:
		text = std::string(scalars[int(scalar)]) + std::to_string(rows) + "x" + std::to_string(columns);
		break;
	case Shape::structure:
		text = structure ? structure->name : "struct";
		break;
	case Shape::object:
		for (const auto &[name, kind] : object_names())
			if (kind == object)
				text = name;
		if (element)
			text += "<" + element->name() + ">";
		break;
	}
	for (const auto size : array)
		text += "[" + std::to_string(size) + "]";
	return text;
}

Parsed parse(const std::vector<Token> &tokens, std::string_view name)
{
	Parsed parsed;
	Parser parser(tokens, parsed.program);
	try {
		parser.run();
	} catch (const ParseError &error) {
		parsed.errors = std::string(name) + "(" + std::to_string(error.line) + "): error: " + error.what() + "\n";
		parsed.failed = true;
	} catch (const std::exception &error) {
		parsed.errors = std::string(name) + ": error: " + error.what() + "\n";
		parsed.failed = true;
	}
	return parsed;
}

}
