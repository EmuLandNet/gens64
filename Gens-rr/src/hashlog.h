// gens64: per-frame emulation state hash log (see hashlog.cpp)
#ifndef GENS64_HASHLOG_H
#define GENS64_HASHLOG_H

int HashLog_Open(const char *fileName);
int HashLog_Active(void);
void HashLog_OnFrame(int rendered);

#endif
