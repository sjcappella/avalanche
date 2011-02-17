/*----------------------------------------------------------------------------------------*/
/*------------------------------------- AVALANCHE ----------------------------------------*/
/*------ Driver. Coordinates other processes, traverses conditional jumps tree.  ---------*/
/*------------------------------- ExecutionManager.cpp -----------------------------------*/
/*----------------------------------------------------------------------------------------*/

/*
   Copyright (C) 2009 Ildar Isaev
      iisaev@ispras.ru

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/


#include "ExecutionManager.h"
#include "Logger.h"
#include "Chunk.h"
#include "OptionConfig.h"
#include "PluginExecutor.h"
#include "STP_Executor.h"
#include "STP_Input.h"
#include "STP_Output.h"
#include "FileBuffer.h"
#include "SocketBuffer.h"
#include "Input.h"
#include "Thread.h"
#include "Monitor.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <string>
#include <vector>
#include <set>

#define N 5

using namespace std;

extern Monitor* monitor;

PoolThread *threads;
extern int thread_num;

bool killed = false;
bool nokill = false;

bool trace_kind;

static Logger *logger = Logger::getLogger();
Input* initial;
int allSockets = 0;
int listeningSocket;
int fifofd;
int memchecks = 0;
Kind kind;
bool is_distributed = false;

vector<Chunk*> report;

pthread_mutex_t add_inputs_mutex;
pthread_mutex_t add_exploits_mutex;
pthread_mutex_t add_bb_mutex;
pthread_mutex_t finish_mutex;
pthread_cond_t finish_cond;

int in_thread_creation = -1;

int distfd;
int agents;

ExecutionManager::ExecutionManager(OptionConfig *opt_config)
{
    DBG(logger, "Initializing plugin manager");

    config      = new OptionConfig(opt_config);
    exploits    = 0;
    divergences = 0;
    is_distributed = opt_config->getDistributed();
    if (thread_num > 0)
    {
      pthread_mutex_init(&add_inputs_mutex, NULL);
      pthread_mutex_init(&add_exploits_mutex, NULL);
      pthread_mutex_init(&add_bb_mutex, NULL);
      pthread_mutex_init(&finish_mutex, NULL);
      pthread_cond_init(&finish_cond, NULL);
    }
  
    if (is_distributed)
    {
      struct sockaddr_in stSockAddr;
      int res;
 
      memset(&stSockAddr, 0, sizeof(struct sockaddr_in));
 
      stSockAddr.sin_family = AF_INET;
      stSockAddr.sin_port = htons(opt_config->getDistPort());
      res = inet_pton(AF_INET, opt_config->getDistHost().c_str(), &stSockAddr.sin_addr);
 
      if (res < 0)
      {
        perror("error: first parameter is not a valid address family");
        exit(EXIT_FAILURE);
      }
      else if (res == 0)
      {
        perror("char string (second parameter does not contain valid ipaddress");
        exit(EXIT_FAILURE);
      }

      distfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

      if (distfd == -1)
      {
        perror("cannot create socket");
        exit(EXIT_FAILURE);
      }
    
      res = connect(distfd, (const struct sockaddr*)&stSockAddr, sizeof(struct sockaddr_in));
 
      if (res < 0)
      {
        perror("error connect failed");
        close(distfd);
        exit(EXIT_FAILURE);
      }  

      LOG(logger, "Connected to server");
      write(distfd, "m", 1);
      read(distfd, &agents, sizeof(int));
   }
}

void ExecutionManager::getTracegrindOptions(vector <string> &plugin_opts)
{
  ostringstream tg_invert_depth;
  tg_invert_depth << "--invertdepth=" << config->getDepth();

  plugin_opts.push_back(tg_invert_depth.str());

  if (config->getDumpCalls())
  {
    plugin_opts.push_back("--dump-file=calldump.log");
  }
  else
  {
    plugin_opts.push_back("--dump-prediction=yes");
  }

  if (config->getCheckDanger())
  {
    plugin_opts.push_back(string("--check-danger=yes"));
  }
  else
  {
    plugin_opts.push_back(string("--check-danger=no"));
  }

  for (int i = 0; i < config->getFuncFilterUnitsNum(); i++)
  {
    plugin_opts.push_back(string("--func-name=") + config->getFuncFilterUnit(i));
  }
  if (config->getFuncFilterFile() != "")
  {
    plugin_opts.push_back(string("--func-filter-file=") + config->getFuncFilterFile());
  }

  if (config->getInputFilterFile() != "")
  {
    plugin_opts.push_back(string("--mask=") + config->getInputFilterFile());
  }

  if (config->getSuppressSubcalls())
  {
    plugin_opts.push_back("--suppress-subcalls=yes");
  }

  if (config->usingSockets())
  {
    ostringstream tg_host;
    tg_host << "--host=" << config->getHost();
    plugin_opts.push_back(tg_host.str());
    ostringstream tg_port;
    tg_port << "--port=" << config->getPort();
    plugin_opts.push_back(tg_port.str());

    plugin_opts.push_back("--replace=yes");
    plugin_opts.push_back("--sockets=yes");
    if (config->getTracegrindAlarm() != 0)
    {
      alarm(config->getTracegrindAlarm());
    }
    killed = false;
  }
  else if (config->usingDatagrams())
  {
    plugin_opts.push_back("--replace=yes");
    plugin_opts.push_back("--datagrams=yes");
    if (config->getTracegrindAlarm() != 0)
    {
      alarm(config->getTracegrindAlarm());
    }
  killed = false;
  }      
  else
  {
    for (int i = 0; i < config->getNumberOfFiles(); i++)
    {
      plugin_opts.push_back(string("--file=") + config->getFile(i));
    }
  }
}

void ExecutionManager::getCovgrindOptions(vector <string> &plugin_opts, string fileNameModifier, bool addNoCoverage)
{
  if (config->usingSockets())
  {
    ostringstream cv_host;
    cv_host << "--host=" << config->getHost();
    plugin_opts.push_back(cv_host.str());

    ostringstream cv_port;
    cv_port << "--port=" << config->getPort();
    plugin_opts.push_back(cv_port.str());
    
    plugin_opts.push_back(string("--replace=replace_data") + fileNameModifier);
    plugin_opts.push_back("--sockets=yes");

    LOG(logger, "setting alarm " << config->getAlarm());
    alarm(config->getAlarm());
    killed = false;
  }
  else if (config->usingDatagrams())
  { 
    plugin_opts.push_back(string("--replace=replace_data") + fileNameModifier);
    plugin_opts.push_back("--datagrams=yes");

    LOG(logger, "setting alarm " << config->getAlarm());
    alarm(config->getAlarm());
    killed = false;
  }
  else
  {
    ostringstream cv_alarm;
    cv_alarm << "--alarm=" << config->getAlarm();
    plugin_opts.push_back(cv_alarm.str());
  }

  string cv_exec_file = string("execution") + fileNameModifier + string(".log");
  plugin_opts.push_back(string("--log-file=") + cv_exec_file);

  if (addNoCoverage)
  {
    plugin_opts.push_back("--no-coverage=yes");
  }
  if (fileNameModifier != string(""))
  {
    plugin_opts.push_back(string("--filename=basic_blocks") + fileNameModifier + string(".log"));
  }
}

void ExecutionManager::dumpExploit(Input *input, FileBuffer* stack_trace, bool info_available, bool same_exploit, int exploit_group)
{
  time_t exploittime;
  time(&exploittime);
  string t = string(ctime(&exploittime));
  REPORT(logger, "Crash detected.");
  LOG(logger, "exploit time: " << t.substr(0, t.size() - 1));
  if (info_available)
  {
    if (!same_exploit)
    {
      stringstream ss(stringstream::in | stringstream::out);
      ss << config->getPrefix() << "stacktrace_" << report.size() - 1 << ".log";
      stack_trace->dumpFile((char*) ss.str().c_str());
      REPORT(logger, "Dumping stack trace to file " << ss.str());
    }
    else
    {
      REPORT(logger, "Bug was detected previously. Stack trace can be found in " 
                     << config->getPrefix() << "stacktrace_" << exploit_group << ".log");
    }
  }
  else
  {
    REPORT(logger, "No stack trace is available.");
  }
  if (config->usingSockets() || config->usingDatagrams())
  {
    stringstream ss(stringstream::in | stringstream::out);
    ss << config->getPrefix() << "exploit_" << exploits;
    REPORT(logger, "Dumping an exploit to file " << ss.str());
    input->dumpExploit((char*) ss.str().c_str(), false);
  }
  else
  {
    for (int i = 0; i < input->files.size(); i++)
    {
      stringstream ss(stringstream::in | stringstream::out);
      ss << config->getPrefix() << "exploit_" << exploits << "_" << i;
      REPORT(logger, "Dumping an exploit to file " << ss.str());
      input->files.at(i)->FileBuffer::dumpFile((char*) ss.str().c_str());
    }
  }
}

bool ExecutionManager::dumpMCExploit(Input* input, const char *exec_log)
{
  FileBuffer* mc_output = new FileBuffer(exec_log);
  char* error = strstr(mc_output->buf, "ERROR SUMMARY: ");
  long errors = -1;
  long definitely_lost = -1;
  long possibly_lost = -1;
  bool res = false;
  if (error != NULL)
  {
    errors = strtol(error + 15, NULL, 10);
  }
  char* leak = NULL;
  if (config->checkForLeaks())
  {
    leak = strstr(mc_output->buf, "definitely lost: ");
    if (leak != NULL)
    {
      definitely_lost = strtol(leak + 17, NULL, 10);
    }
    leak = strstr(mc_output->buf, "possibly lost: ");
    if (leak != NULL)
    {
      possibly_lost = strtol(leak + 15, NULL, 10);
    }
  }
  if ((errors > 0) || (((definitely_lost != -1) || (possibly_lost != -1)) && !killed))
  {
    time_t memchecktime;
    time(&memchecktime);
    string t = string(ctime(&memchecktime));
    REPORT(logger, "Error detected.");
    LOG(logger, "memcheck error time: " << t.substr(0, t.size() - 1));   
    if (config->usingSockets() || config->usingDatagrams())
    {
      stringstream ss(stringstream::in | stringstream::out);
      ss << config->getPrefix() << "memcheck_" << memchecks;
      REPORT(logger, "Dumping input for memcheck error to file " << ss.str());
      input->dumpExploit((char*) ss.str().c_str(), false);
    }
    else
    {
      for (int i = 0; i < input->files.size(); i++)
      {
        stringstream ss(stringstream::in | stringstream::out);
        ss << config->getPrefix() << "memcheck_" << memchecks << "_" << i;
        REPORT(logger, "Dumping input for memcheck error to file " << ss.str());
        input->files.at(i)->FileBuffer::dumpFile((char*) ss.str().c_str());
      }
    }
    res = true; 
  }
  delete mc_output;
  return res;
}

int ExecutionManager::calculateScore(string fileNameModifier)
{
  bool enable_mutexes = (fileNameModifier != string(""));
  int res = 0;
  int fd = open((string("basic_blocks") + fileNameModifier + string(".log")).c_str(), O_RDWR);
  if (fd != -1)
  {
    struct stat fileInfo;
    fstat(fd, &fileInfo);
    int size = fileInfo.st_size / sizeof(long);
    if (size > 0)
    {
      unsigned long basicBlockAddrs[size];
      read(fd, basicBlockAddrs, fileInfo.st_size);
      close(fd);
      if (enable_mutexes) pthread_mutex_lock(&add_bb_mutex);
      for (int i = 0; i < size; i++)
      {
        if (basicBlocksCovered.find(basicBlockAddrs[i]) == basicBlocksCovered.end())
        {
          res++;
        }
        if(thread_num < 1)
        {
          basicBlocksCovered.insert(basicBlockAddrs[i]);
        }
        else
        {
          delta_basicBlocksCovered.insert(basicBlockAddrs[i]);
        }
      }
      if (enable_mutexes) pthread_mutex_unlock(&add_bb_mutex);
    }
  }
  else
  {
    ERR(logger, "Error opening file " << string("basic_blocks") + fileNameModifier + string(".log"));
  }
  return res;
}

int ExecutionManager::checkAndScore(Input* input, bool addNoCoverage, string fileNameModifier, bool first_run)
{
  if (config->usingSockets() || config->usingDatagrams())
  {
    input->dumpExploit("replace_data", false, fileNameModifier.c_str());
  }
  else
  {
    input->dumpFiles(NULL, fileNameModifier.c_str());
  }
  vector<string> plugin_opts;
  getCovgrindOptions(plugin_opts, fileNameModifier, addNoCoverage);

  string cv_exec_file = string("execution") + fileNameModifier + string(".log");
  
  vector <string> new_prog_and_args = config->getProgAndArg();
  
  if (fileNameModifier != string("") && !(config->usingSockets()) && !(config->usingDatagrams()))
  {
    for (int i = 0; i < new_prog_and_args.size(); i ++)
    {
      for (int j = 0; j < input->files.size(); j ++)
      {
        if (!strcmp(new_prog_and_args[i].c_str(), input->files.at(j)->name))
        {
          new_prog_and_args[i].append(fileNameModifier);
        }
      }
    }
  }
  PluginExecutor plugin_exe(config->getDebug(), config->getTraceChildren(), config->getValgrind(),
                            new_prog_and_args, plugin_opts, addNoCoverage ? COVGRIND : kind);
  new_prog_and_args.clear();
  plugin_opts.clear();
  bool enable_mutexes = (config->getSTPThreads() != 0) && !first_run;
  int thread_index = (fileNameModifier == string("")) ? 0 : atoi(fileNameModifier.substr(1).c_str());
  monitor->setState(CHECKER, time(NULL), thread_index);
  int exitCode = plugin_exe.run(thread_index);
  monitor->addTime(time(NULL), thread_index);
  FileBuffer* mc_output;
  bool infoAvailable = false;
  bool sameExploit = false;
  int exploit_group = 0;
  if (enable_mutexes) pthread_mutex_lock(&add_exploits_mutex);
  bool has_crashed = (exitCode == -1);
  if (!thread_num)
  {
    has_crashed = has_crashed && !killed;
  }
  else
  {
    has_crashed = has_crashed && !(((ParallelMonitor*) monitor)->getAlarmKilled(thread_index));
  }
  if (has_crashed)
  {
    int chunk_file_num = (config->usingSockets() || config->usingDatagrams()) ? (-1) : (input->files.size());
    FileBuffer* cv_output = new FileBuffer(cv_exec_file.c_str());
    infoAvailable = cv_output->filterCovgrindOutput();
    if (infoAvailable)
    {
      for (vector<Chunk*>::iterator it = report.begin(); it != report.end(); it++, exploit_group++)
      {
        if (((*it)->getTrace() != NULL) && (*(*it)->getTrace() == *cv_output))
        {
          sameExploit = true;
          (*it)->addGroup(exploits, chunk_file_num);
          break;
        }
      }
      if (!sameExploit) 
      {
        Chunk* ch;
        ch = new Chunk(cv_output, exploits, chunk_file_num);
        report.push_back(ch);
      }
    }
    else
    {
      Chunk* ch;
      ch = new Chunk(NULL, exploits, chunk_file_num);
      report.push_back(ch);
    }
    dumpExploit(input, cv_output, infoAvailable, sameExploit, exploit_group);
    exploits ++;
    delete cv_output;
  }
  else if (config->usingMemcheck() && !addNoCoverage)
  {
    if (dumpMCExploit(input, cv_exec_file.c_str()))
    {
      memchecks ++;
    }
  }
  if (enable_mutexes) pthread_mutex_unlock(&add_exploits_mutex);
  int result = 0;
  if (!addNoCoverage)
  {
    result = calculateScore(fileNameModifier);
  }
  return result;
}

int ExecutionManager::checkDivergence(Input* first_input, int score)
{
  int divfd = open("divergence.log", O_RDWR);
  if (divfd != -1)
  {
    bool divergence;
    read(divfd, &divergence, sizeof(bool));
    if (divergence)
    {
      int d;
      read(divfd, &d, sizeof(int));
      DBG(logger, "divergence at depth " << d << "\n");
      if (config->usingSockets() || config->usingDatagrams())
      {
        stringstream ss(stringstream::in | stringstream::out);
        ss << config->getPrefix() << "divergence_" << divergences;
        LOG(logger, "dumping divergent input to file " << ss.str());
        first_input->parent->dumpExploit((char*) ss.str().c_str(), false);
      }
      else
      {
        for (int i = 0; i < first_input->parent->files.size(); i++)
        {
          stringstream ss(stringstream::in | stringstream::out);
          ss << config->getPrefix() << "divergence_" << divergences << "_" << i;
          LOG(logger, "dumping divergent input to file " << ss.str());
          first_input->parent->files.at(i)->FileBuffer::dumpFile((char*) ss.str().c_str());
        }
      }
      divergences++;
      DBG(logger, "with startdepth=" << first_input->parent->startdepth << " and invertdepth=" << config->getDepth() << "\n");
      close(divfd);
      if (score == 0) 
      {
        if (is_distributed)
        {
          talkToServer();
        }
        return 1;
      }
    }
  }
  return 0;
}

void ExecutionManager::updateInput(Input* input)
{
  int fd = open("replace_data", O_RDWR);
  int socketsNum;
  read(fd, &socketsNum, sizeof(int));
  for (int i = 0; i < socketsNum; i++)
  {
    int chunkSize;
    read(fd, &chunkSize, sizeof(int));
    if (i >= input->files.size())
    {
      input->files.push_back(new SocketBuffer(i, chunkSize));
    }
    else if (input->files.at(i)->size < chunkSize)
    {
      input->files.at(i)->size = chunkSize;
      input->files.at(i)->buf = (char*) realloc(input->files.at(i)->buf, chunkSize);
      memset(input->files.at(i)->buf, 0, chunkSize);
    }
    read(fd, input->files.at(i)->buf, chunkSize);
  }
  close(fd);
}

void alarmHandler(int signo)
{
  LOG(logger, "time is out");
  if (!nokill)
  {
    monitor->handleSIGALARM();
    killed = true;
    DBG(logger, "Time out. Valgrind is going to be killed");
  }
  signal(SIGALRM, alarmHandler);
}

void* process_query(void* data)
{
  PoolThread* actor = (PoolThread*) data;
  ExecutionManager* this_pointer = (ExecutionManager*) (Thread::getSharedData("this_pointer"));
  multimap<Key, Input*, cmp>* inputs = (multimap <Key, Input*, cmp> *) (Thread::getSharedData("inputs"));
  Input* first_input = (Input*) (Thread::getSharedData("first_input"));
  bool* actual = (bool*) (Thread::getSharedData("actual"));
  long first_depth = (long) (Thread::getSharedData("first_depth"));
  long depth = (long) (actor->getPrivateData("depth"));
  int cur_tid = actor->getCustomTID();
  this_pointer->processQuery(first_input, actual, first_depth, depth, cur_tid);
  return NULL;
}

int ExecutionManager::processQuery(Input* first_input, bool* actual, unsigned long first_depth, unsigned long cur_depth, unsigned int thread_index)
{
  string cur_trace_log = (trace_kind) ? string("curtrace") : string("curdtrace");
  string input_modifier = string("");
  if (thread_index)
  {
    ostringstream input_modifier_s;
    input_modifier_s << "_" << thread_index;
    input_modifier = input_modifier_s.str();
  }
  cur_trace_log += input_modifier + string(".log");
  STP_Input si;
  si.setFile(cur_trace_log.c_str());
  STP_Executor stp_exe(getConfig()->getDebug(), getConfig()->getValgrind());        
  monitor->setState(STP, time(NULL), thread_index);
  STP_Output *out = stp_exe.run(&si, thread_index);
  monitor->addTime(time(NULL), thread_index);
  if (out == NULL)
  {
    if (!monitor->getKilledStatus())
    {
      ERR(logger, "STP has encountered an error");
      FileBuffer f(cur_trace_log.c_str());
      ERR(logger, cur_trace_log.c_str() << ":\n" << string(f.buf));
    }
  }
  else if (out->getFile() != NULL)
  {
    FileBuffer f(out->getFile());
    DBG(logger, "Thread #" << thread_index << ": stp output:\n" << string(f.buf));
    Input* next = new Input();
    int st_depth = first_input->startdepth;
    for (int k = 0; k < first_input->files.size(); k++)
    { 
      FileBuffer* fb = first_input->files.at(k);
      fb = fb->forkInput(out->getFile());
      if (fb == NULL)
      {
        delete next;
        next = NULL;
        break;
      }
      else
      {
        next->files.push_back(fb);
      }
    }
    if (next != NULL)
    {
      next->startdepth = st_depth + cur_depth + 1;
      next->prediction = new bool[st_depth + cur_depth];
      for (int j = 0; j < st_depth + cur_depth - 1; j++)
      {
        next->prediction[j] = actual[j];
      }
      next->prediction[st_depth + cur_depth - 1] = !actual[st_depth + cur_depth - 1];
      next->predictionSize = st_depth + cur_depth;
      next->parent = first_input;
      int score = checkAndScore(next, !trace_kind, input_modifier);
      if (trace_kind)
      {
        if (thread_index)
        {
          LOG(logger, "Thread #" << thread_index << ": score=" << score << "\n");
          pthread_mutex_lock(&add_inputs_mutex);
        }
        else
        {
          LOG(logger, "score=" << score << "\n");
        }
        inputs.insert(make_pair(Key(score, first_depth + cur_depth + 1), next));
        if (thread_index) 
        {
          pthread_mutex_unlock(&add_inputs_mutex);
        }
      }
    }
  }
  if (out != NULL) delete out;
  return 1;
}

int ExecutionManager::processTraceParallel(Input * first_input, unsigned long first_depth)
{
  int actualfd = open("actual.log", O_RDWR);
  int actual_length;
  if (config->getDepth() == 0)
  {
    read(actualfd, &actual_length, sizeof(int));
  }
  else
  {
    actual_length = first_input->startdepth - 1 + config->getDepth();
  }
  bool actual[actual_length];
  read(actualfd, actual, actual_length * sizeof(bool));
  close(actualfd);
  int active_threads = thread_num;
  long depth = 0;
  FileBuffer *trace = new FileBuffer(((trace_kind) ? "trace.log" : "dangertrace.log"));
  Thread::clearSharedData();
  Thread::addSharedData((void*) &inputs, string("inputs"));
  Thread::addSharedData((void*) first_input, string("first_input"));
  Thread::addSharedData((void*) first_depth, string("first_depth"));
  Thread::addSharedData((void*) actual, string("actual"));
  Thread::addSharedData((void*) this, string("this_pointer"));
  char* query = trace->buf;
  while((query = strstr(query, "QUERY(FALSE);")) != NULL)
  {
    depth ++;
    query ++;
  }
  for (int j = 0; j < ((depth < thread_num) ? depth : thread_num); j ++)
  {
    threads[j].setCustomTID(j + 1);
    threads[j].setPoolSync(&finish_mutex, &finish_cond, &active_threads);
  }
  STP_Output* outputs[depth];
  int thread_counter;
  pool_data external_data[depth];
  for (int i = 0; i < depth; i ++)
  {
    pthread_mutex_lock(&finish_mutex);
    if (active_threads == 0) 
    {
      pthread_cond_wait(&finish_cond, &finish_mutex);
    }
    for (thread_counter = 0; thread_counter < thread_num; thread_counter ++) 
    {
      if (threads[thread_counter].getStatus())
      {
        break;
      }
    }
    if (threads[thread_counter].getStatus() == PoolThread::FREE)
    {
      threads[thread_counter].waitForThread();
    }
    active_threads --;
    threads[thread_counter].addPrivateData((void*) i, string("depth"));
    external_data[i].work_func = process_query;
    external_data[i].data = &(threads[thread_counter]);
    ostringstream cur_trace;
    if (trace_kind)
    {
      cur_trace << "curtrace_";
    }
    else
    {
      cur_trace << "curdtrace_";
    } 
    cur_trace << thread_counter + 1 << ".log";
    trace->cutQueryAndDump(cur_trace.str().c_str(), trace_kind);
    in_thread_creation = thread_counter;
    threads[thread_counter].setStatus(PoolThread::BUSY);
    threads[thread_counter].createThread(&(external_data[i]));
    in_thread_creation = -1;
    pthread_mutex_unlock(&finish_mutex);
  }
  for (int i = 0; i < ((depth < thread_num) ? depth : thread_num); i ++)
  {
    threads[i].waitForThread();
  }
  delete trace;
  return depth;
}

int ExecutionManager::processTraceSequental(Input* first_input, unsigned long first_depth)
{
  int actualfd = open("actual.log", O_RDWR);
  int actual_length, depth = 0;
  if (config->getDepth() == 0)
  {
    read(actualfd, &actual_length, sizeof(int));
  }
  else
  {
    actual_length = first_input->startdepth - 1 + config->getDepth();
  }
  bool* actual = new bool[actual_length];
  read(actualfd, actual, actual_length * sizeof(bool));
  close(actualfd);
  if (config->getCheckDanger())
  {
    int cur_depth = 0;
    trace_kind = false;
    FileBuffer dtrace("dangertrace.log");
    char* dquery;
    while ((dquery = strstr(dtrace.buf, "QUERY(FALSE)")) != NULL)
    {
      dtrace.cutQueryAndDump("curdtrace.log");
      processQuery(first_input, actual, first_depth, cur_depth ++);
    }
  }
  trace_kind = true;
  FileBuffer trace("trace.log");
  char* query;
  while ((query = strstr(trace.buf, "QUERY(FALSE)")) != NULL)
  {
    depth++;
    trace.cutQueryAndDump("curtrace.log", true);
    processQuery(first_input, actual, first_depth, depth - 1);
  }
  delete []actual;
  return depth;
}

void dummy_handler(int signo)
{

}

int ExecutionManager::requestNonZeroInput()
{
  multimap<Key, Input*, cmp>::iterator it = --(inputs.end());
  int best_score = it->first.score;
  if ((best_score == 0) && config->getAgent())
  {
    LOG(logger, "All inputs have zero score: requesting new input");
    signal(SIGUSR2, dummy_handler);
    kill(getppid(), SIGUSR1);
    pause();
    int descr = open("startdepth.log", O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
    int startdepth = 0;
    read(descr, &startdepth, sizeof(int));
    close(descr);
    if (startdepth > 0)
    {
      return startdepth;
    }
    config->setNotAgent();
    inputs.erase(it);
  }
  else
  {
    inputs.erase(it);
  }
  return 0;
}

void ExecutionManager::run()
{
    DBG(logger, "Running execution manager");
    int runs = 0;
    if (config->usingMemcheck())
    {
      kind = MEMCHECK;
    }
    else
    {
      kind = COVGRIND;
    }

    initial = new Input();
    if (!config->usingSockets() && !config->usingDatagrams())
    {
      for (int i = 0; i < config->getNumberOfFiles(); i++)
      {
        initial->files.push_back(new FileBuffer((char*) config->getFile(i).c_str()));
      }
    }
    else 
    {
      if (config->getAgent())
      {
        updateInput(initial);
      }
      signal(SIGALRM, alarmHandler);
    }
    initial->startdepth = config->getStartdepth();
    int score = checkAndScore(initial, false, "", true);
    basicBlocksCovered.insert(delta_basicBlocksCovered.begin(), delta_basicBlocksCovered.end());
    LOG(logger, "score=" << score);
    inputs.insert(make_pair(Key(score, 0), initial));
    bool delete_fi;
    
    while (!inputs.empty()) 
    {
      delete_fi = false;
      REPORT(logger, "Starting iteration " << runs);
      LOG(logger, "inputs.size()=" << inputs.size());
      delta_basicBlocksCovered.clear();
      multimap<Key, Input*, cmp>::iterator it = --(inputs.end());
      Input* fi = it->second;
      unsigned int scr = it->first.score;
      unsigned int dpth = it->first.depth;
      LOG(logger, "selected next input with score " << scr);

      if (config->usingSockets() || config->usingDatagrams())
      {
        fi->dumpExploit("replace_data", true);
      }
      else
      {
        fi->dumpFiles();
      }

      ostringstream tg_depth;
      vector<string> plugin_opts;
      bool newInput = false;

      int startdepth = requestNonZeroInput();
      if (startdepth)
      {
        tg_depth << "--startdepth=" << startdepth;
        newInput = true;
      }
      else
      {
        tg_depth << "--startdepth=" << fi->startdepth;
        plugin_opts.push_back(tg_depth.str());
        if (runs > 0)
        {
          plugin_opts.push_back("--check-prediction=yes");
        }
      }
  
      getTracegrindOptions(plugin_opts);

      PluginExecutor plugin_exe(config->getDebug(), config->getTraceChildren(), config->getValgrind(), config->getProgAndArg(), plugin_opts, TRACEGRIND);
      plugin_opts.clear();
      if (config->getTracegrindAlarm() == 0)
      {
        nokill = true;
      }
      time_t start_time = time(NULL);
      monitor->setState(TRACER, start_time);
      int exitCode = plugin_exe.run();
      monitor->addTime(time(NULL));
      if (config->getTracegrindAlarm() == 0)
      {      
        nokill = false;
      }
      if (config->usingSockets() || config->usingDatagrams())
      {
        updateInput(fi);
      }

      if (exitCode == -1)
      {
        LOG(logger, "failure in tracegrind");
      }

      if (config->getDebug() && (runs > 0) && !newInput)
      {
        if (checkDivergence(fi, scr))
        {
          runs ++;
          continue;
        }
      }
 
      if (config->getDumpCalls())
      {
        break;
      }
      int depth = 0;
      if (thread_num)
      {
        if (config->getCheckDanger())
        {
          trace_kind = false;
          depth = processTraceParallel(fi, dpth);
        }
        trace_kind = true;
        depth = processTraceParallel(fi, dpth);
      }

      else
      {
        depth = processTraceSequental(fi, dpth);
      }
        
      if (depth == 0)
      {
        LOG(logger, "no QUERY's found\n");
      }
      runs++;
      if (delete_fi)
      {
        if (initial != fi)
        {
          delete fi;
        }
      }
      basicBlocksCovered.insert(delta_basicBlocksCovered.begin(), delta_basicBlocksCovered.end());
      if (is_distributed)
      {
        talkToServer();
      }
    }
    if (!(config->usingSockets()) && !(config->usingDatagrams()))
    {
      initial->dumpFiles();
    }
}

void writeToSocket(int fd, const void* b, size_t count)
{
  char* buf = (char*) b;
  size_t sent = 0;
  while (sent < count)
  {
    size_t s = write(fd, buf + sent, count - sent);
    if (s == -1)
    {
      throw "error writing to socket";
    }
    sent += s;
  }
}

void readFromSocket(int fd, const void* b, size_t count)
{
  char* buf = (char*) b;
  size_t received = 0;
  while (received < count)
  {
    size_t r = read(fd, buf + received, count - received);
    if (r == 0)
    {
      throw "connection is down";
    }
    if (r == -1)
    {
      throw "error reading from socket";
    }
    received += r;
  }
}

void ExecutionManager::talkToServer()
{
  try
  {
    NET(logger, "Communicating with server");
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(distfd, &readfds);
    struct timeval timer;
    timer.tv_sec = 0;
    timer.tv_usec = 0;
    select(distfd + 1, &readfds, NULL, NULL, &timer);
    int limit = config->getProtectMainAgent() ? N * agents : 1;
    while (FD_ISSET(distfd, &readfds)) 
    {
      char c = '\0';
      readFromSocket(distfd, &c, 1);
      if (c == 'a')
      {
        NET(logger, "Sending options and data");
        writeToSocket(distfd, "r", 1); 
        //sending "r"(responding) before data - this is to have something different from "q", so that server
        //can understand that main avalanche finished normally
        int size;
        readFromSocket(distfd, &size, sizeof(int));
        while (size > 0)
        {
          if (inputs.size() <= limit)
          {
            break;
          }
          multimap<Key, Input*, cmp>::iterator it = --inputs.end();
          it--;
          Input* fi = it->second;
          int filenum = fi->files.size();
          writeToSocket(distfd, &filenum, sizeof(int));
          bool sockets = config->usingSockets();
          writeToSocket(distfd, &sockets, sizeof(bool));
          bool datagrams = config->usingDatagrams();
          writeToSocket(distfd, &datagrams, sizeof(bool));
          for (int j = 0; j < fi->files.size(); j ++)
          {
            FileBuffer* fb = fi->files.at(j);
            if (!config->usingDatagrams() && ! config->usingSockets())
            {
              int namelength = config->getFile(j).length();
              writeToSocket(distfd, &namelength, sizeof(int));
              writeToSocket(distfd, config->getFile(j).c_str(), namelength);
            }
            writeToSocket(distfd, &(fb->size), sizeof(int));
            writeToSocket(distfd, fb->buf, fb->size);
          }
          writeToSocket(distfd, &fi->startdepth, sizeof(int));
          int depth = config->getDepth();
          writeToSocket(distfd, &depth, sizeof(int));
          unsigned int alarm = config->getAlarm();
          writeToSocket(distfd, &alarm, sizeof(int));
          unsigned int tracegrindAlarm = config->getTracegrindAlarm();
          writeToSocket(distfd, &tracegrindAlarm, sizeof(int));
          int threads = config->getSTPThreads();
          writeToSocket(distfd, &threads, sizeof(int));

          int progArgsNum = config->getProgAndArg().size();
          writeToSocket(distfd, &progArgsNum, sizeof(int));

          bool useMemcheck = config->usingMemcheck();
          writeToSocket(distfd, &useMemcheck, sizeof(bool));
          bool leaks = config->checkForLeaks();
          writeToSocket(distfd, &leaks, sizeof(bool));
          bool traceChildren = config->getTraceChildren();
          writeToSocket(distfd, &traceChildren, sizeof(bool));
          bool checkDanger = config->getCheckDanger();
          writeToSocket(distfd, &checkDanger, sizeof(bool));
          bool debug = config->getDebug();
          writeToSocket(distfd, &debug, sizeof(bool));
          bool verbose = config->getVerbose();
          writeToSocket(distfd, &verbose, sizeof(bool));
          bool suppressSubcalls = config->getSuppressSubcalls();
          writeToSocket(distfd, &suppressSubcalls, sizeof(bool));
          bool STPThreadsAuto = config->getSTPThreadsAuto();
          writeToSocket(distfd, &STPThreadsAuto, sizeof(bool));

          if (sockets)
          {
            string host = config->getHost();
            int length = host.length();
            writeToSocket(distfd, &length, sizeof(int));
            writeToSocket(distfd, host.c_str(), length);
            unsigned int port = config->getPort();
            writeToSocket(distfd, &port, sizeof(int));
          }

          if (config->getInputFilterFile() != "")
          {
            FileBuffer mask(config->getInputFilterFile().c_str());
            writeToSocket(distfd, &mask.size, sizeof(int));
            writeToSocket(distfd, mask.buf, mask.size);
          }
          else
          {
            int z = 0;
            writeToSocket(distfd, &z, sizeof(int));
          }

          int funcFilters = config->getFuncFilterUnitsNum();
          writeToSocket(distfd, &funcFilters, sizeof(int));
          for (int i = 0; i < config->getFuncFilterUnitsNum(); i++)
          {
            string f = config->getFuncFilterUnit(i);
            int length = f.length();
            writeToSocket(distfd, &length, sizeof(int));
            writeToSocket(distfd, f.c_str(), length);
          }
          if (config->getFuncFilterFile() != "")
          {
            FileBuffer filter(config->getFuncFilterFile().c_str());
            writeToSocket(distfd, &filter.size, sizeof(int));
            writeToSocket(distfd, filter.buf, filter.size);
          }
          else
          {
            int z = 0;
            writeToSocket(distfd, &z, sizeof(int));
          }

          for (vector<string>::const_iterator it = config->getProgAndArg().begin(); it != config->getProgAndArg().end(); it++)
          {
            int argsSize = it->length();
            writeToSocket(distfd, &argsSize, sizeof(int));
            writeToSocket(distfd, it->c_str(), argsSize);
          }
          if (it->second != initial)
          {
            delete it->second;
          }
          inputs.erase(it);
          size--;
        }
        while (size > 0)
        {
          int tosend = 0;
          writeToSocket(distfd, &tosend, sizeof(int));
          size--;
        }
      }
      else if (c == 'g')
      {
        writeToSocket(distfd, "r", 1);
        //sending "r"(responding) before data - this is to have something different from "q", so that server
        //can understand that main avalanche finished normally
        int size;
        readFromSocket(distfd, &size, sizeof(int));
        while (size > 0)
        {
          if (inputs.size() <= limit)
          { 
            break;
          }
          NET(logger, "Sending input");
          multimap<Key, Input*, cmp>::iterator it = --inputs.end();
          it--;
          Input* fi = it->second;
          for (int j = 0; j < fi->files.size(); j ++)
          {
            FileBuffer* fb = fi->files.at(j);
            writeToSocket(distfd, &(fb->size), sizeof(int));
            writeToSocket(distfd, fb->buf, fb->size);
          }
          writeToSocket(distfd, &fi->startdepth, sizeof(int));
          if (it->second != initial)
          {
            delete it->second;
          }
          inputs.erase(it);
          size--;
        }
        while (size > 0)
        {
          int tosend = 0;
          writeToSocket(distfd, &tosend, sizeof(int));
          size--;
        }
      }
      else
      {
        int tosend = 0;
        writeToSocket(distfd, &tosend, sizeof(int));
      }
      FD_ZERO(&readfds);
      FD_SET(distfd, &readfds);
      select(distfd + 1, &readfds, NULL, NULL, &timer);      
    }
  }
  catch (const char* msg)
  {
    NET(logger, "Connection with server lost");
    NET(logger, "Continuing work in local mode");
    is_distributed = false;
  }
}

ExecutionManager::~ExecutionManager()
{
    DBG(logger, "Destructing plugin manager");

    if (is_distributed)
    {
      write(distfd, "q", 1);
      shutdown(distfd, SHUT_RDWR);
      close(distfd);
    }

    if (thread_num > 0)
    {
      pthread_mutex_destroy(&add_inputs_mutex);
      pthread_mutex_destroy(&add_exploits_mutex);
      pthread_mutex_destroy(&add_bb_mutex);
      pthread_mutex_destroy(&finish_mutex);
    }

    delete config;
}
