#pragma once

#ifndef interface
#define interface struct
#endif

interface IPhysicShellCreator
{
public:
	virtual void CreatePhysicsShell() = 0;
};
