/*----------------------------------------------------------------------------------------*/
/*------------------------------------- AVALANCHE ----------------------------------------*/
/*------ Driver. Coordinates other processes, traverses conditional jumps tree.  ---------*/
/*-------------------------------------- Input.h -----------------------------------------*/
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

#ifndef __INPUT__H__
#define __INPUT__H__

#include <vector>

class FileBuffer;

class Input
{
public:
  Input();
  void dumpFiles(char* name = NULL, const char* name_modifier = "");
  void dumpExploit(const char* name, bool predict, const char* name_modifier = "");

  std::vector<FileBuffer*> files;
  int startdepth;
  Input* parent;
  bool* prediction;
  int predictionSize;
};

#endif
