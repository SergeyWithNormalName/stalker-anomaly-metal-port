#pragma once

#define WIDEN_HELPER(x) L ## x
#define WIDEN(x) WIDEN_HELPER(x)
#define PIX_EVENT(Name) dxPixEventWrapper pixEvent##Name(WIDEN(#Name))

class dxPixEventWrapper
{
public:
    dxPixEventWrapper(LPCWSTR wszName);
    ~dxPixEventWrapper();
}; 