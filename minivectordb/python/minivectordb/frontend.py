"""SQLGlot AST lowering only: no schema lookup, SQL execution or vector decoding."""
from sqlglot import exp
from sqlglot.errors import SqlglotError
from sqlglot.tokens import TokenType

from .dialect import MYSQL, statement_tokens, unsupported, vector_index
from .errors import DatabaseError


def checked(node, cls, *allowed):
    """Every visited node admits only the fields represented by the frozen IR."""
    if type(node) is not cls:
        unsupported(f"unsupported SQL node: {type(node).__name__}")
    for key, value in node.args.items():
        if key not in allowed and value is not None and value is not False and value != []:
            unsupported(f"unsupported {cls.__name__} clause: {key}")
    return node


def name(node):
    checked(node, exp.Identifier, "this", "quoted")
    if not node.this or "\0" in node.this:
        unsupported("invalid identifier")
    return node.this


def normalized(value):
    return ''.join(chr(ord(c) + 32) if 'A' <= c <= 'Z' else c for c in value)


def table(node):
    checked(node, exp.Table, "this")
    return name(node.this)


def select_table(node):
    checked(node, exp.Table, "this", "alias")
    result = name(node.this)
    qualifiers = {normalized(result)}
    alias = node.args.get("alias")
    if alias is not None:
        checked(alias, exp.TableAlias, "this")
        qualifiers.add(normalized(name(alias.this)))
    return result, qualifiers


def projection_sources(sql, tokens, count):
    depth = 0
    from_index = None
    for i, token in enumerate(tokens[1:], 1):
        if token.token_type == TokenType.L_PAREN:
            depth += 1
        elif token.token_type == TokenType.R_PAREN:
            depth -= 1
        elif depth == 0 and token.token_type == TokenType.FROM:
            from_index = i
            break
    if from_index is None:
        unsupported("SELECT requires FROM")
    ranges, start, depth = [], 1, 0
    for i in range(1, from_index):
        token = tokens[i]
        if token.token_type == TokenType.L_PAREN:
            depth += 1
        elif token.token_type == TokenType.R_PAREN:
            depth -= 1
        elif token.token_type == TokenType.COMMA and depth == 0:
            ranges.append((start, i))
            start = i + 1
    ranges.append((start, from_index))
    if len(ranges) != count or any(a >= b for a, b in ranges):
        unsupported("cannot preserve projection source text")
    return [sql[tokens[a].start:tokens[b - 1].end + 1] for a, b in ranges]


def literal(node):
    negative = type(node) is exp.Neg
    if negative:
        checked(node, exp.Neg, "this")
        node = node.this
    checked(node, exp.Literal, "this", "is_string")
    if node.is_string:
        if negative:
            unsupported("negative string literal")
        return {"kind": "string", "text": node.this}
    if not node.this or any(c not in "0123456789" for c in node.this):
        unsupported("only decimal integer and string literals are supported")
    return {"kind": "integer", "text": ("-" if negative else "") + node.this}


def column(node, qualifiers=None):
    checked(node, exp.Column, "this", "table")
    qualifier = node.args.get("table")
    if qualifier is not None and (qualifiers is None or
                                  normalized(name(qualifier)) not in qualifiers):
        raise DatabaseError("unknown_column", "column qualifier does not match the target table")
    return name(node.this)


def expression(node, qualifiers=None):
    if type(node) is exp.Paren:
        checked(node, exp.Paren, "this")
        return expression(node.this, qualifiers)
    if type(node) is exp.Column:
        return {"kind": "column", "name": column(node, qualifiers)}
    if type(node) is exp.Star:
        checked(node, exp.Star)
        return {"kind": "star"}
    if type(node) in (exp.Literal, exp.Neg):
        return literal(node)
    if type(node) is exp.Anonymous:
        checked(node, exp.Anonymous, "this", "expressions")
        if node.this.upper() != "L2_DISTANCE" or len(node.expressions) != 2:
            unsupported("only L2_DISTANCE(column, string) is supported")
        vector = literal(node.expressions[1])
        if vector["kind"] != "string":
            unsupported("L2_DISTANCE vector argument must be a string")
        return {"kind": "l2_distance", "column": column(node.expressions[0], qualifiers), "vector": vector}
    unsupported(f"unsupported expression: {type(node).__name__}")


def order_expression(node, qualifiers):
    if type(node) is exp.Literal and not node.is_string and node.this.isdigit():
        position = int(node.this)
        if position > 2**64 - 1:
            raise DatabaseError("type_error", "ORDER BY position is outside the projection")
        return {"kind": "ordinal", "position": position}
    if type(node) is exp.Neg and type(node.this) is exp.Literal and not node.this.is_string:
        unsupported("negative ORDER BY ordinal")
    return expression(node, qualifiers)


def where(node, qualifiers=None):
    if node is None:
        return {}
    checked(node, exp.Where, "this")

    def comparisons(item):
        if type(item) is exp.Paren:
            checked(item, exp.Paren, "this")
            return comparisons(item.this)
        if type(item) is exp.And:
            checked(item, exp.And, "this", "expression")
            return comparisons(item.this) + comparisons(item.expression)
        operators = {exp.EQ: "=", exp.LT: "<", exp.LTE: "<=", exp.GT: ">", exp.GTE: ">="}
        if type(item) not in operators:
            unsupported("WHERE supports only AND comparisons")
        checked(item, type(item), "this", "expression")
        return [{"left": expression(item.this, qualifiers), "operator": operators[type(item)],
                 "right": expression(item.expression, qualifiers)}]

    return {"where": comparisons(node.this)}


def unsigned(node):
    value = literal(node)
    if value["kind"] != "integer" or value["text"].startswith("-"):
        unsupported("expected unsigned integer")
    number = int(value["text"])
    if number > 2**64 - 1:
        unsupported("unsigned integer exceeds uint64")
    return number


def column_schema(node):
    checked(node, exp.ColumnDef, "this", "kind", "constraints")
    dtype = checked(node.args.get("kind"), exp.DataType, "this", "expressions", "nested")
    if dtype.args.get("nested"):
        unsupported("nested types are unsupported")
    types = {exp.DataType.Type.BIGINT: "bigint", exp.DataType.Type.VARCHAR: "varchar",
             exp.DataType.Type.VECTOR: "vector"}
    if dtype.this not in types:
        unsupported("unsupported column type")
    result = {"name": name(node.this), "type": types[dtype.this]}
    params = dtype.expressions
    if result["type"] == "bigint":
        if params:
            unsupported("BIGINT length is unsupported")
    else:
        if len(params) != 1:
            unsupported("VARCHAR and VECTOR require one length")
        checked(params[0], exp.DataTypeParam, "this")
        result["length"] = unsigned(params[0].this)
    constraints = node.args.get("constraints") or []
    if constraints:
        if len(constraints) != 1:
            unsupported("only inline PRIMARY KEY is supported")
        constraint = checked(constraints[0], exp.ColumnConstraint, "kind")
        checked(constraint.args.get("kind"), exp.PrimaryKeyColumnConstraint)
        result["primary_key"] = True
    return result


def lower(node, sql=None, tokens=None):
    if type(node) is exp.Create:
        checked(node, exp.Create, "this", "kind")
        if node.args.get("kind") != "TABLE":
            unsupported("only CREATE TABLE and CREATE VECTOR INDEX are supported")
        schema = checked(node.this, exp.Schema, "this", "expressions")
        columns = []
        table_primary_keys = []
        for item in schema.expressions:
            if type(item) is exp.ColumnDef:
                columns.append(column_schema(item))
                continue
            checked(item, exp.PrimaryKey, "expressions", "include", "options")
            checked(item.args.get("include"), exp.IndexParameters)
            if item.args.get("options") or len(item.expressions) != 1 or type(item.expressions[0]) is not exp.Identifier:
                raise DatabaseError("type_error", "PRIMARY KEY requires exactly one column")
            table_primary_keys.append(name(item.expressions[0]))
        for primary_key in table_primary_keys:
            matches = [c for c in columns if normalized(c["name"]) == normalized(primary_key)]
            if not matches:
                raise DatabaseError("unknown_column", "PRIMARY KEY column does not exist")
            if matches[0].get("primary_key"):
                raise DatabaseError("type_error", "duplicate PRIMARY KEY declaration")
            matches[0]["primary_key"] = True
        return {"op": "create_table", "table": table(schema.this), "columns": columns}
    if type(node) is exp.Insert:
        checked(node, exp.Insert, "this", "expression")
        target = node.this
        result = {"op": "insert"}
        if type(target) is exp.Schema:
            checked(target, exp.Schema, "this", "expressions")
            result["columns"] = [name(c) for c in target.expressions]
            target = target.this
        result["table"] = table(target)
        values = checked(node.expression, exp.Values, "expressions")
        rows = []
        for row in values.expressions:
            checked(row, exp.Tuple, "expressions")
            rows.append([literal(v) for v in row.expressions])
        result["rows"] = rows
        return result
    if type(node) is exp.Select:
        checked(node, exp.Select, "expressions", "from_", "where", "order", "limit")
        source = checked(node.args.get("from_"), exp.From, "this")
        table_name, qualifiers = select_table(source.this)
        sources = projection_sources(sql, tokens, len(node.expressions))
        projections = []
        for item, source_text in zip(node.expressions, sources):
            projection = {}
            aliased = type(item) is exp.Alias
            if aliased:
                checked(item, exp.Alias, "this", "alias")
                projection["alias"] = name(item.args["alias"])
                item = item.this
            projection["expression"] = expression(item, qualifiers)
            if not aliased and type(item) is not exp.Star:
                projection["display_text"] = source_text
            projections.append(projection)
        result = {"op": "select", "table": table_name, "projections": projections,
                  **where(node.args.get("where"), qualifiers)}
        if node.args.get("order") is not None:
            order = checked(node.args["order"], exp.Order, "expressions")
            items = []
            for item in order.expressions:
                checked(item, exp.Ordered, "this", "desc", "nulls_first")
                # SQLGlot supplies MySQL's implicit NULL placement; IR has no NULL.
                items.append({"expression": order_expression(item.this, qualifiers),
                              "descending": bool(item.args.get("desc"))})
            result["order_by"] = items
        if node.args.get("limit") is not None:
            limit = checked(node.args["limit"], exp.Limit, "expression")
            result["limit"] = unsigned(limit.expression)
        return result
    if type(node) is exp.Update:
        checked(node, exp.Update, "this", "expressions", "where")
        table_name = table(node.this)
        qualifiers = {normalized(table_name)}
        assignments = []
        for item in node.expressions:
            checked(item, exp.EQ, "this", "expression")
            assignments.append({"column": column(item.this, qualifiers), "value": literal(item.expression)})
        return {"op": "update", "table": table_name, "assignments": assignments,
                **where(node.args.get("where"), qualifiers)}
    if type(node) is exp.Delete:
        checked(node, exp.Delete, "this", "where")
        table_name = table(node.this)
        return {"op": "delete", "table": table_name,
                **where(node.args.get("where"), {normalized(table_name)})}
    transactions = {exp.Transaction: "begin", exp.Commit: "commit", exp.Rollback: "rollback"}
    if type(node) in transactions:
        checked(node, type(node))
        if node.args.get("chain") is not None:
            unsupported("transaction chaining clauses are unsupported")
        return {"op": transactions[type(node)]}
    unsupported(f"unsupported statement: {type(node).__name__}")


def parse_statement(sql: str) -> dict:
    """Return frozen, Python-primitive IR for exactly one supported statement."""
    if not isinstance(sql, str):
        unsupported("SQL must be a string")
    try:
        # The native boundary stores UTF-8, not isolated JSON surrogate units.
        sql.encode("utf-8")
        tokens = statement_tokens(sql)
        index = vector_index(tokens)
        if index is not None:
            return index
        # Parse token objects, never split or rewrite SQL text. Command fallback
        # cannot bypass lower()'s root whitelist.
        nodes = MYSQL.parser().parse(tokens, sql)
        if len(nodes) != 1 or nodes[0] is None:
            unsupported("expected exactly one SQL statement")
        if type(nodes[0]) in (exp.Transaction, exp.Commit, exp.Rollback):
            # SQLGlot discards some ROLLBACK suffixes; forbid any unrepresented
            # transaction tokens before lowering the otherwise valid AST.
            words = [token.text.upper() for token in tokens]
            if words not in (["BEGIN"], ["START", "TRANSACTION"], ["COMMIT"], ["ROLLBACK"]):
                unsupported("unsupported transaction clause")
        return lower(nodes[0], sql, tokens)
    except (SqlglotError, RecursionError, ValueError) as error:
        unsupported(f"invalid or unsupported SQL: {error}")
