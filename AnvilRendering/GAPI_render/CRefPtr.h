//
// CRefPtr.h
// Copyright 1992 - 2006 Dennis Robinson (www.menasoft.com)
//
// General object ref mechanism. smart pointers

#ifndef _INC_CRefPtr_H
#define _INC_CRefPtr_H
#if _MSC_VER >= 1000
#pragma once
#endif // _MSC_VER >= 1000

#include "CLogBase.h"
#include "CThreadLockedLong.h"

#define CRefObjBase_STATIC_VAL 0x01000000
#define PTR_CAST(t,p) (dynamic_cast <t*>(p))

class TAKSI_LINK CRefObjBase 
{
	// base class for some derived object that is to be referenced.
	// NOTE: These objects are normally CHeapObject, but NOT ALWAYS ! (allow static versions)
	// NOTE: These objects may also be common based on IUnknown. we may use IRefPtr<> for this..
	// friend class template CRefPtr;
public:
	CRefObjBase( int iRefCount = 0 ) :
		m_nRefCount(iRefCount)
	{
	}
	virtual ~CRefObjBase()
	{
		// OK not to call StaticDestruct i suppose.
		ASSERT( get_RefCount()==0 || get_RefCount()==CRefObjBase_STATIC_VAL );
	}

	int get_RefCount() const
	{
		return m_nRefCount.m_lValue;
	}

	// do something the first time someone uses this. (load from disk etc)
	virtual void OnFirstRef() 
	{
		// This is almost NEVER used!
	}
	// do something when no-one wants this anymore. cache or delete?
	virtual void OnFinalRelease()
	{
		// Some derived class may want to cache the object even if the ref count goes to 0 ?
		//  use 
		// NOTE: Obviously this should NEVER be called for a static or stack based object.
		//  use StaticConstruct() for these.
		delete this;
	}
	void InternalAddRef() 
	{
		// NOTE: This can throw exception if the OnFirstRef fails.
#ifdef _DEBUG
		ASSERT( PTR_CAST(CRefObjBase,this));
#endif
		int iRefCount = m_nRefCount.Inc();
		if ( iRefCount == 1 )
		{
			OnFirstRef();
		}
	}
	void InternalRelease()
	{
#ifdef _DEBUG
		ASSERT( PTR_CAST(CRefObjBase,this));
#endif
		int iRefCount = m_nRefCount.Dec();
		if ( iRefCount == 0 )
		{
			OnFinalRelease();
		}
		else
		{
			ASSERT(iRefCount>0);
		}
	}

	// COM IUnknown compliant methods.
	virtual ULONG __stdcall AddRef(void) // like COM IUnknown::AddRef
	{
		InternalAddRef();
		return get_RefCount();
	}
	virtual ULONG __stdcall Release(void) // like COM IUnknown::Release
	{
		int iRefCount = get_RefCount();
		InternalRelease();	// this could get deleted here!
		return iRefCount-1;
	}

#if 0 // def _DEBUG
#define IncRefCount() AddRef()	// always go through the COM interface!
#define DecRefCount() Release()
#else
#define IncRefCount() InternalAddRef()
#define DecRefCount() InternalRelease()
#endif

	bool IsStaticConstruct() const
	{
		return m_nRefCount.m_lValue >= CRefObjBase_STATIC_VAL;
	}
	void StaticConstruct()
	{
		// If this is static, not dynamic. Call this in parents constructor or main (if global).
		ASSERT(get_RefCount()==0); 
		//IncRefCount();
		m_nRefCount.m_lValue = CRefObjBase_STATIC_VAL;
	}
	void StaticDestruct()
	{
		// static objects can fix themselves this way.
		ASSERT(get_RefCount()==CRefObjBase_STATIC_VAL);
		m_nRefCount.m_lValue = 0;
	}
private:
	CThreadLockedLong m_nRefCount;	// count the number of refs. Multi-Thread safe.
};

// Template for a type specific Ref

template<class _TYPE> 
class TAKSI_LINK CRefPtr
{
	// Smart pointer to an object. like "com_ptr_t"
	// Just a ref to the object of some type.
	// _TYPE must be based on CRefObjBase
public:
	// Construct and destruct
	CRefPtr() :
		m_pObj(NULL)
	{
	}
	CRefPtr( _TYPE* pObj )
	{
		// NOTE: = assignment will auto destroy prev and use this constructor.
		SetFirstRefObj(pObj);
	}
#if 1 // _MSC_VER < 1300	// VC 7.0 does not like this? 
	CRefPtr( const CRefPtr<_TYPE>& ref )
	{
		// using the assingment auto constructor is not working so use this.
		SetFirstRefObj( ref.get_RefObj());
	}
#endif
	CRefPtr( _TYPE* pObj, DWORD dwWaitMS ) 
	{
		// This is to fake out CThreadLockPtr in single thread mode.
		UNREFERENCED_PARAMETER(dwWaitMS);
		SetFirstRefObj(pObj);
	}
	~CRefPtr()
	{
		ReleaseRefObj();
	}

	bool IsValidRefObj() const
	{
#ifdef _DEBUG
		if ( m_pObj )
		{
			ASSERT( PTR_CAST(_TYPE,m_pObj));
			ASSERT( PTR_CAST(CRefObjBase,m_pObj));
		}
#endif
		return m_pObj != NULL;
	}
	_TYPE** get_PPtr()
	{
		ASSERT(m_pObj==NULL);
		return &m_pObj;
	}
	_TYPE* get_RefObj() const
	{
		return( m_pObj );
	}
	void put_RefObj( _TYPE* pObj )
	{
		ReleaseRefObj();
		SetFirstRefObj(pObj);
	}
	_TYPE* DetachRefObj()
	{
		// Pass the ref outside the smart pointer system. for use with COM interfaces.
		_TYPE* pObj = m_pObj;
		m_pObj = NULL;	// NOT ReleaseRefObj();
		return pObj;
	}
	void ReleaseRefObj()
	{
		_TYPE* pObj = m_pObj;
		if ( pObj )
		{
#ifdef _DEBUG
			ASSERT( PTR_CAST(_TYPE,pObj));
#endif
			m_pObj = NULL;	// make sure possible destructors called in DecRefCount don't reuse this.
			pObj->DecRefCount();	// this might delete this ?
		}
	}

	// Assignment ops.
	CRefPtr<_TYPE>& operator = ( _TYPE* pRef )
	{
		if ( pRef != m_pObj )
		{
			put_RefObj( pRef );
		}
		return *this;
	}
    CRefPtr<_TYPE>& operator = ( const CRefPtr<_TYPE>& ref )
    {
		return operator=(ref.m_pObj);
	}

	operator CRefPtr<_TYPE>&() 
	{
		return *this;
	}

	operator const CRefPtr<_TYPE>&() const
	{
		return *this;
	}

#if(1)
	// explicit ref type conversion - to remove redundant casts
	// will work only for property related types
	template<class T2> operator CRefPtr<T2>() const
	{
		return CRefPtr<T2>( m_pObj ); // this will automatically give an error if classes are unrelated
	}
#endif

	// Accessor ops.
	// NOTE: These are dangerous ! they dont inc the ref count for use !!!
	operator _TYPE*() const 
	{ return( m_pObj ); }

    _TYPE& operator * () const 
	{ ASSERT(m_pObj); return *m_pObj; }

	_TYPE* operator -> () const 
	{ ASSERT(m_pObj); return( m_pObj ); }

	// Comparison ops
	bool operator!() const
	{
		return( m_pObj == NULL );
	}
	bool operator != ( /*const*/ _TYPE* pRef ) const
	{
		return( pRef != m_pObj );
	}
	bool operator == ( /*const*/ _TYPE* pRef ) const
	{
		return( pRef == m_pObj );
	}
#if 0
	bool operator == ( _TYPE* pRef ) const
	{
		return( pRef == m_pObj );
	}
#endif

protected:
	void SetFirstRefObj( _TYPE* pObj )
	{
		// NOTE: IncRefCount can throw !
		if ( pObj )
		{
#ifdef _DEBUG
			ASSERT( PTR_CAST(_TYPE,pObj));
#endif
			pObj->IncRefCount();
		}
		m_pObj = pObj;
	}
private:
	_TYPE* m_pObj;	// object we are referring to MUST be based on CRefObjBase
};

// The lowest (untypechecked) smart pointer.
template class TAKSI_LINK CRefPtr<CRefObjBase>;
typedef TAKSI_LINK CRefPtr<CRefObjBase> CRefBasePtr;

// Similar to COM QueryInterface() this checks to see if the class is supported.
#define REF_CAST(_DSTCLASS,p) PTR_CAST(_DSTCLASS,(p).get_RefObj())
#define REFS_CAST(_DSTCLASS,p) STATIC_CAST(_DSTCLASS,(p).get_RefObj())

#endif // _INC_CRefObj_H
