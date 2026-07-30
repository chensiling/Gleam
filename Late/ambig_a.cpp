// Translation unit A: defines a file-static "ambig" that IS called.
static int ambig()
{
    return 100;
}

int LateAmbigA()
{
    return ambig();
}
