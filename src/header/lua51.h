#pragma once

namespace lua51 {
    struct lua_State;
    using LuaCFunction = int(__cdecl*)(lua_State*);

    constexpr int globalsIndex = -10002;
    constexpr int typeNil = 0;
    constexpr int typeBoolean = 1;
    constexpr int typeNumber = 3;
    constexpr int typeString = 4;
    constexpr int typeTable = 5;
    constexpr int typeFunction = 6;

    bool init();
    bool ready();
    lua_State* getState();
    int getTop(lua_State* state);
    void setTop(lua_State* state, int index);
    void createTable(lua_State* state, int arrayCount, int fieldCount);
    void getField(lua_State* state, int index, const char* name);
    void setField(lua_State* state, int index, const char* name);
    void rawGetIndex(lua_State* state, int index, int item);
    void getGlobal(lua_State* state, const char* name);
    void setGlobal(lua_State* state, const char* name);
    void pushString(lua_State* state, const char* value);
    void pushNumber(lua_State* state, double value);
    void pushBoolean(lua_State* state, bool value);
    void pushFunction(lua_State* state, LuaCFunction function);
    int type(lua_State* state, int index);
    const char* toString(lua_State* state, int index);
    double toNumber(lua_State* state, int index);
    bool toBoolean(lua_State* state, int index);
    int pcall(lua_State* state, int arguments, int results, int errorFunction);
}
