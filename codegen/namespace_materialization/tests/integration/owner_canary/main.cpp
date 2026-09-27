import bioseq.codegen.namespace_materialization.owner_canary;

int main()
{
    namespace owner_canary =
        bioseq::codegen::namespace_materialization::owner_canary;

    if (owner_canary::owner_increment(1) != 11) return 1;
    if (owner_canary::owner_increment(1LL) != 101LL) return 2;
    if (owner_canary::owner_scale(2) != 14) return 3;
    if (owner_canary::owner_scale(2LL) != 104LL) return 4;
    return 0;
}
