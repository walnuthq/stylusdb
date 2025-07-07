#pragma once

#include <lldb/API/SBCommandInterpreter.h>

class CallTraceStartCommand : public lldb::SBCommandPluginInterface {
public:
  bool DoExecute(lldb::SBDebugger debugger, char **command,
                 lldb::SBCommandReturnObject &result) override;
};

class CallTraceStopCommand : public lldb::SBCommandPluginInterface {
public:
  bool DoExecute(lldb::SBDebugger debugger, char **command,
                 lldb::SBCommandReturnObject &result) override;
};

class FormatEnableCommand : public lldb::SBCommandPluginInterface {
public:
  bool DoExecute(lldb::SBDebugger debugger, char **command,
                 lldb::SBCommandReturnObject &result) override;
};

bool RegisterWalnutCommands(lldb::SBCommandInterpreter &interpreter);
