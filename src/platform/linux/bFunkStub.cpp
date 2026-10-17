// EA's bFunk sent debug packets to a developer PC over the dev-kit link. Nothing listens for
// them on Linux, so calls are dropped.
void bFunkCallASync(const char *, int, const void *, int) {}
