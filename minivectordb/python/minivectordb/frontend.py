"""SQLGlot AST lowering only: no schema lookup, SQL execution or vector decoding."""
from sqlglot import exp
from sqlglot.errors import SqlglotError

from .dialect import MYSQL, statement_tokens, unsupported, vector_index


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


def table(node):
    checked(node, exp.Table, "this")
    return name(node.this)


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


def column(node):
    checked(node, exp.Column, "this")
    return name(node.this)


def expression(node):
    if type(node) is exp.Column:
        return {"kind": "column", "name": column(node)}
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
        return {"kind": "l2_distance", "column": column(node.expressions[0]), "vector": vector}
    unsupported(f"unsupported expression: {type(node).__name__}")


def where(node):
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
        return [{"left": expression(item.this), "operator": operators[type(item)],
                 "right": expression(item.expression)}]

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


def lower(node):
    if type(node) is exp.Create:
        checked(node, exp.Create, "this", "kind")
        if node.args.get("kind") != "TABLE":
            unsupported("only CREATE TABLE and CREATE VECTOR INDEX are supported")
        schema = checked(node.this, exp.Schema, "this", "expressions")
        return {"op": "create_table", "table": table(schema.this),
                "columns": [column_schema(c) for c in schema.expressions]}
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
        projections = []
        for item in node.expressions:
            projection = {}
            if type(item) is exp.Alias:
                checked(item, exp.Alias, "this", "alias")
                projection["alias"] = name(item.args["alias"])
                item = item.this
            projection["expression"] = expression(item)
            projections.append(projection)
        result = {"op": "select", "table": table(source.this), "projections": projections,
                  **where(node.args.get("where"))}
        if node.args.get("order") is not None:
            order = checked(node.args["order"], exp.Order, "expressions")
            items = []
            for item in order.expressions:
                checked(item, exp.Ordered, "this", "desc", "nulls_first")
                # SQLGlot supplies MySQL's implicit NULL placement; IR has no NULL.
                items.append({"expression": expression(item.this), "descending": bool(item.args.get("desc"))})
            result["order_by"] = items
        if node.args.get("limit") is not None:
            limit = checked(node.args["limit"], exp.Limit, "expression")
            result["limit"] = unsigned(limit.expression)
        return result
    if type(node) is exp.Update:
        checked(node, exp.Update, "this", "expressions", "where")
        assignments = []
        for item in node.expressions:
            checked(item, exp.EQ, "this", "expression")
            assignments.append({"column": column(item.this), "value": literal(item.expression)})
        return {"op": "update", "table": table(node.this), "assignments": assignments,
                **where(node.args.get("where"))}
    if type(node) is exp.Delete:
        checked(node, exp.Delete, "this", "where")
        return {"op": "delete", "table": table(node.this), **where(node.args.get("where"))}
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
        return lower(nodes[0])
    except (SqlglotError, RecursionError, ValueError) as error:
        unsupported(f"invalid or unsupported SQL: {error}")
