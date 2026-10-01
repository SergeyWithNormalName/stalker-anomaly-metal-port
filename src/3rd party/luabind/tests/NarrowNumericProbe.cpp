#include <cassert>
#include <cstring>
#include <cstdio>
#include <limits>

#include <luabind/luabind.hpp>

namespace
{
using Converter = luabind::detail::primitive_converter<luabind::detail::Direction::lua_to_cpp>;

template <typename T>
T convert(lua_State* state)
{
    Converter converter;
    return converter.apply(state, luabind::detail::by_value<T>{}, 1);
}

struct Case
{
    double number;
    int u8, s8, u16, s16;
};

volatile int observedTeam = -1, observedSquad = -1, observedGroup = -1;

// This separate typed callee exercises the optimized ARM64 byte-argument ABI,
// including implicit promotion in the receiving method, like ChangeTeam.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void receive(unsigned char team, unsigned char squad, unsigned char group)
{
    observedTeam = team;
    observedSquad = squad;
    observedGroup = group;
}

int dispatch(lua_State* state)
{
    Converter converter;
    void (*callback)(unsigned char, unsigned char, unsigned char) = receive;
    callback(converter.apply(state, luabind::detail::by_value<unsigned char>{}, 1),
        converter.apply(state, luabind::detail::by_value<unsigned char>{}, 2),
        converter.apply(state, luabind::detail::by_value<unsigned char>{}, 3));
    return 0;
}

bool run(lua_State* state)
{
    const Case cases[] = {
        {0.0, 0, 0, 0, 0}, {6664.0, 8, 8, 6664, 6664},
        {-1.0, 255, -1, 65535, -1}, {256.0, 0, 0, 256, 256},
        {-256.0, 0, 0, 65280, -256}, {65536.0, 0, 0, 0, 0},
        {65535.0, 255, -1, 65535, -1}, {32768.0, 0, 0, 32768, -32768},
        {127.9, 127, 127, 127, 127}, {-129.9, 127, 127, 65407, -129},
        {-0.9, 0, 0, 0, 0}, {255.75, 255, -1, 255, 255},
        {std::numeric_limits<double>::quiet_NaN(), 0, 0, 0, 0},
        {std::numeric_limits<double>::infinity(), 0, 0, 0, 0},
        {-std::numeric_limits<double>::infinity(), 0, 0, 0, 0},
        {std::numeric_limits<double>::max(), 0, 0, 0, 0},
        {std::numeric_limits<double>::lowest(), 0, 0, 0, 0},
    };
    for (const Case& value : cases)
    {
        lua_settop(state, 0);
        lua_pushnumber(state, value.number);
        const int expectedChar = std::numeric_limits<char>::is_signed ? value.s8 : value.u8;
        if (convert<unsigned char>(state) != value.u8 || convert<signed char>(state) != value.s8 ||
            convert<unsigned short>(state) != value.u16 || convert<short>(state) != value.s16 ||
            convert<char>(state) != expectedChar)
        {
            std::fprintf(stderr, "narrow conversion mismatch for %.17g\n", value.number);
            return false;
        }
        if (Converter::match(state, luabind::detail::by_value<unsigned char>{}, 1) != 0)
            return false;
    }
    lua_settop(state, 0);
    lua_pushnil(state);
    if (convert<char>(state) || convert<signed char>(state) || convert<unsigned char>(state) ||
        convert<short>(state) || convert<unsigned short>(state))
        return false;
    lua_settop(state, 0);
    lua_pushstring(state, "6664");
    if (Converter::match(state, luabind::detail::by_value<unsigned char>{}, 1) != -1)
        return false;

    lua_settop(state, 0);
    lua_pushcfunction(state, dispatch);
    lua_pushnumber(state, 17.0);
    lua_pushnumber(state, 6664.0);
    lua_pushnumber(state, 257.0);
    if (lua_pcall(state, 3, 0, 0) != 0 ||
        observedTeam != 17 || observedSquad != 8 || observedGroup != 1)
    {
        std::fprintf(stderr, "typed callback observed %d/%d/%d\n",
            observedTeam, observedSquad, observedGroup);
        return false;
    }
    lua_settop(state, 0);
    lua_pushnumber(state, 6664.0);
    if (convert<int>(state) != 6664 || convert<unsigned int>(state) != 6664u ||
        convert<long long>(state) != 6664ll || convert<unsigned long long>(state) != 6664ull)
        return false;
    std::printf("LUABIND_NARROW_NUMERIC PASS cases=%zu types=5 nil=0 callback=17/8/1 wide=unchanged char_signed=%u\n",
        sizeof(cases) / sizeof(cases[0]), static_cast<unsigned>(std::numeric_limits<char>::is_signed));
    return true;
}
} // namespace

int main()
{
    lua_State* state = luaL_newstate();
    if (!state) return 2;
    const bool passed = run(state);
    lua_close(state);
    return passed ? 0 : 1;
}
