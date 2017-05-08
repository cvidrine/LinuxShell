/**
 * File: stsh.cc
 * -------------
 * Defines the entry point of the stsh executable.
 */

#include "stsh-parser/stsh-parse.h"
#include "stsh-parser/stsh-readline.h"
#include "stsh-parser/stsh-parse-exception.h"
#include "stsh-signal.h"
#include "stsh-job-list.h"
#include "stsh-job.h"
#include "stsh-process.h"
#include <cstring>
#include <iostream>
#include <string>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>  // for fork
#include <signal.h>  // for kill
#include <sys/wait.h>
#include <assert.h>
using namespace std;

static STSHJobList joblist; // the one piece of global data we need so signal handlers can access it

/**
 * Function: handleBuiltin
 * -----------------------
 * Examines the leading command of the provided pipeline to see if
 * it's a shell builtin, and if so, handles and executes it.  handleBuiltin
 * returns true if the command is a builtin, and false otherwise.
 */
static const string kSupportedBuiltins[] = {"quit", "exit", "fg", "bg", "slay", "halt", "cont", "jobs"};
static const size_t kNumSupportedBuiltins = sizeof(kSupportedBuiltins)/sizeof(kSupportedBuiltins[0]);

static void updateJobList(pid_t pid, STSHProcessState state);
static void waitForForeground(pid_t pid);

static void runProcess(const pipeline& pipeline, bool foreground){
   string name = (foreground)?"fg":"bg";
   if(pipeline.commands[0].tokens[0] == NULL) throw STSHException((string)"Usage: "+name+ (string)" <jobid>.");
   int jobNum = atoi(pipeline.commands[0].tokens[0]); 
   if(jobNum == 0) throw STSHException((string)"Usage: " +name+ (string)" <jobid>.");
   if(!joblist.containsJob(jobNum)) throw STSHException(name+(string)" "+(string)pipeline.commands[0].tokens[0]+": No such job.");
   STSHJob& job = joblist.getJob(jobNum);
   job.setState((foreground)?kForeground:kBackground);
   pid_t groupID = job.getGroupID();
   kill(-groupID, SIGCONT);
   if(foreground)waitForForeground(groupID);
}

int checkZero(char *token, string name){//Checks if input was actualy zero or if it was just malformed.
    if(isdigit(token[0])) return 0;
    throw STSHException((string)"Usage: " +name+ (string)" <jobid> <index> | >pid>");
    return -1;
}

int findNumArgs(const pipeline& pipeline, string name){
    //Error checks for 0 or more than 2 arguments
    if(pipeline.commands[0].tokens[0] == NULL || pipeline.commands[0].tokens[2] != NULL){
        throw STSHException((string)"Usage: " +name+ (string)" <jobid> <index> | <pid>");
        return -1;
    }
    //Ensures that 1st argument is an int.
    if(atoi(pipeline.commands[0].tokens[0]) == 0){
        if(checkZero(pipeline.commands[0].tokens[0], name) == -1) return -1;
    }

    if(pipeline.commands[0].tokens[1] == NULL) return 1; //Checks for presence of second argument
    
    //Ensures second argument is an int.
    if(atoi(pipeline.commands[0].tokens[1]) == 0){
        if(checkZero(pipeline.commands[0].tokens[1], name) == -1) return -1;
    }

    return 2;
}   

int getSig(string name){
    if(name == "Slay") return SIGKILL;
    if(name == "Halt") return SIGTSTP;
    if(name == "Cont") return SIGCONT;
    return 0;
}

static void processCommand(const pipeline& pipeline, string name){
    int numArgs = findNumArgs(pipeline, name);
    if(numArgs == -1) return;

    //Search by PID.
    if(numArgs == 1){
        pid_t pid = atoi(pipeline.commands[0].tokens[0]);

        if(!joblist.containsProcess(pid)){
            throw STSHException((string)"No process with pid "+(string)pipeline.commands[0].tokens[0]+ (string)".");
            return;
        }

        STSHProcess& process = joblist.getJobWithProcess(pid).getProcess(pid);
        if((name == "Halt" && process.getState()==kStopped) || (name == "Cont" && process.getState() == kRunning)) return; 
        kill(pid, getSig(name));

    } 
    //Search by Job number and index.
    if(numArgs ==2){
        int jobNum = atoi(pipeline.commands[0].tokens[0]);

        if(!joblist.containsJob(jobNum)){
            throw STSHException((string)"No job with id of " + pipeline.commands[0].tokens[0] + (string)".");
            return;
        }

        vector<STSHProcess>& processes = joblist.getJob(jobNum).getProcesses();
        size_t index = atoi(pipeline.commands[0].tokens[1]);
        if(processes.size()-1 > index){
            throw STSHException((string)"Job " +(string)pipeline.commands[0].tokens[0]+ (string)" doesn't have a process at index " + (string)pipeline.commands[0].tokens[1] + (string)".");
            return;
        }
        if((name == "Halt" && processes[index].getState()==kStopped) || (name=="Cont" && processes[index].getState() == kRunning)) return;
        kill(processes[index].getID(), getSig(name));
    }
}

static bool handleBuiltin(const pipeline& pipeline) {
  const string& command = pipeline.commands[0].command;
  auto iter = find(kSupportedBuiltins, kSupportedBuiltins + kNumSupportedBuiltins, command);
  if (iter == kSupportedBuiltins + kNumSupportedBuiltins) return false;
  size_t index = iter - kSupportedBuiltins;

  switch (index) {
  case 0:
  case 1: exit(0);
  case 2: runProcess(pipeline, true); break; 
  case 3: runProcess(pipeline, false); break;
  case 4: processCommand(pipeline, "Slay"); break;
  case 5: processCommand(pipeline, "Halt"); break;
  case 6: processCommand(pipeline, "Cont"); break;
  case 7: cout << joblist; break;
  default: throw STSHException("Internal Error: Builtin command not supported."); // or not implemented yet
  }
  
  return true;
}

static void updateJobList(pid_t pid, STSHProcessState state){
    if(!joblist.containsProcess(pid)) return;
    STSHJob& job = joblist.getJobWithProcess(pid);
    if(!job.containsProcess(pid)) return;
    STSHProcess& process = job.getProcess(pid);
    if(process.getState() == state) return;
    process.setState(state);
    joblist.synchronize(job);
}

void handleChild(int sig){
    int status;
    pid_t pid = waitpid(-1, &status, WUNTRACED | WCONTINUED | WNOHANG);
    if(WIFEXITED(status) || WIFSIGNALED(status)){
        updateJobList(pid, kTerminated);
    }else if(WIFCONTINUED(status)){
        updateJobList(pid, kRunning);
    }else if(WIFSTOPPED(status)){
        updateJobList(pid, kStopped);
    }
}


void handleInturrupt(int sig){
    if(!joblist.hasForegroundJob()) return;
    STSHJob& foregroundJob = joblist.getForegroundJob();
    for(STSHProcess& process: foregroundJob.getProcesses()) kill(process.getID(), sig);
}



/**
 * Function: installSignalHandlers
 * -------------------------------
 * Installs user-defined signals handlers for four signals
 * (once you've implemented signal handlers for SIGCHLD, 
 * SIGINT, and SIGTSTP, you'll add more installSignalHandler calls) and 
 * ignores two others.
 */
static void installSignalHandlers() {
  installSignalHandler(SIGQUIT, [](int sig) { exit(0); });
  installSignalHandler(SIGTTIN, SIG_IGN);
  installSignalHandler(SIGTTOU, SIG_IGN);
  installSignalHandler(SIGCHLD, handleChild); 
  installSignalHandler(SIGINT, handleInturrupt);
  installSignalHandler(SIGTSTP, handleInturrupt);
}
static void waitForForeground(pid_t pid){
   sigset_t set;
  sigset_t oldset;
  sigemptyset(&set);
  sigaddset(&set, SIGCHLD);
  sigprocmask(SIG_SETMASK, &set, &oldset);


 if(!joblist.containsProcess(pid))return;
  while(true){
      STSHJob &job = joblist.getJobWithProcess(pid);
      if(!joblist.hasForegroundJob())return;
      
      sigsuspend(&oldset);//Wait for child to finish and execute child handler.

      if(!joblist.hasForegroundJob())break;
      STSHJob& foregroundJob = joblist.getForegroundJob();
      if(job.getNum()!= foregroundJob.getNum()) break;
  }
  sigprocmask(SIG_UNBLOCK, &set, NULL);
}

static void printBgSummary(STSHJob& job){
   cout <<"[" << job.getNum() << "]";
   for(STSHProcess &process: job.getProcesses()) cout << " "<<process.getID();
   cout <<endl;
}


static void executeCommand(STSHJob& job, const pipeline& p, int index){
      char *args[kMaxArguments +2];
      args[0] = (char *)p.commands[index].command;
      for(size_t i=0; i<kMaxArguments+2; i++) args[i+1] = p.commands[index].tokens[i];
      execvp(p.commands[index].command, args);
      throw STSHException((string)p.commands[index].command + (string)": Command not found.");
      exit(0);
}


/**
 * Function: createJob
 * -------------------
 * Creates a new job on behalf of the provided pipeline.
 */
static void createJob(const pipeline& p) {
 STSHJob& job =  joblist.addJob((p.background)?kBackground:kForeground);
  int fdsArr[p.commands.size()][2];
  int redirect[2];
  if(!p.input.empty()) redirect[0] = open(p.input.c_str(), O_RDONLY);
  if(!p.output.empty()) redirect[1] = open(p.output.c_str(), O_CREAT | O_TRUNC | O_RDWR ,0644);
  for(size_t i=0; i<p.commands.size(); i++){
      bool pipeIn = true; bool pipeOut = true;
      if(i == 0) pipeIn = false;
      if(i == p.commands.size()-1) pipeOut = false;

      if(pipeOut){
          int fds[2];
          pipe(fds);
          fdsArr[i][0] = fds[0];
          fdsArr[i][1] = fds[1];
      }

      pid_t pid = fork();
      if(pid == 0) {  
          if(pipeOut){
              close(fdsArr[i][0]);
              dup2(fdsArr[i][1], STDOUT_FILENO);
              close(fdsArr[i][1]);
          } else if (!p.output.empty()){
            dup2(redirect[1], STDOUT_FILENO);
            close(redirect[1]);
          }

          if(pipeIn){
              close(fdsArr[i-1][1]);
              dup2(fdsArr[i-1][0], STDIN_FILENO);
              close(fdsArr[i-1][0]);
          } else if(!p.input.empty()){
            dup2(redirect[0], STDIN_FILENO);
            close(redirect[0]);            
          }

          setpgid(pid, job.getGroupID());
          executeCommand(job, p, i);
      }
    if(i >1){
        close(fdsArr[i-1][0]);
        close(fdsArr[i-1][1]);
    }
    if(!pipeIn) close(fdsArr[i][1]);
    if(!pipeOut) close(fdsArr[i-1][0]);
    setpgid(pid, job.getGroupID());
    job.addProcess(STSHProcess(pid, p.commands[i]));
  }

  for(size_t i=0; i< p.commands.size(); i++){ //Close pipes in parent.
      close(fdsArr[i][0]);
      close(fdsArr[i][1]);
  }

  if(tcsetpgrp(STDIN_FILENO, job.getGroupID()) == -1 && errno != ENOTTY) throw STSHException("Error handing terminal control to child process.");
  if(!p.background){
    waitForForeground(job.getGroupID());
    tcsetpgrp(STDIN_FILENO, getpid());
  } else{
    printBgSummary(job);
  }
}

/**
 * Function: main
 * --------------
 * Defines the entry point for a process running stsh.
 * The main function is little more than a read-eval-print
 * loop (i.e. a repl).  
 */
int main(int argc, char *argv[]) {
  pid_t stshpid = getpid();
  installSignalHandlers();
  rlinit(argc, argv);
  while (true) {
    string line;
    if (!readline(line)) break;
    if (line.empty()) continue;
    try {
      pipeline p(line);
      bool builtin = handleBuiltin(p);
      if (!builtin) createJob(p);
    } catch (const STSHException& e) {
      cerr << e.what() << endl;
      if (getpid() != stshpid) exit(0); // if exception is thrown from child process, kill it
    }
  }

  return 0;
}
