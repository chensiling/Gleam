// Translation unit B: defines a file-static "ambig" that is NOT called.
// This creates the ambiguous symbol shape for SYM-1 testing.
static int ambig()
{
    return 200;
}

int LateAmbigB()
{
    return ambig();
}
