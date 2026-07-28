// expected-failure: BIOSEQ_SQL_E_ATTRIBUTE_NAMESPACE

import bioseq.codegen.sql.annotation_model;

[[bioseq::sql::function({
    .volatility = bioseq::sql::volatility::immutable,
})]]
int invalid_nested_namespace(int value) {
    return value;
}
