// gens64: automation for sync regression testing (see hashlog.cpp)
#ifndef GENS64_HASHLOG_H
#define GENS64_HASHLOG_H

int HashLog_Open(const char *fileName);
int HashLog_Active(void);
void HashLog_OnFrame(int rendered);

struct typeMovie;
void Gens64_SetMovieState(const char *stateFile);
void Gens64_SelectMovieState(typeMovie *movie, const char *movieFile);
void Gens64_SetStopFrame(long frame);
void Gens64_SetBatch(int on);
void Gens64_AfterFrame(int rendered);

#endif
