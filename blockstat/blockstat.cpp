/*
Calculates common block on the filesystem (handy with REFS cloning)
If only one file is supplied, dumps the location of all the fragments making up the file on the volume


Flow
main -> parse arguments
	 -> check for files (supplied on the cli)
	 -> check for files in the input file (supplied by -i)
	 -> check for files by piping (strongly recommended not to use)

If one file -> dumpfile(
				-> vcnnums(
				-> printsingle( or xmlprintsingle( to output the result (depending on -x)

If multiple file	-> comparefiles(
						-> vcnnums( per file (update an int map which keeps how many  any cluster is shared by the inputing file)
						-> use printcompare( or xmlprintcompare( to output the result

						
						

						
Powershell calling can be done:
	$exe = "C:\refsc\blockstat.exe"
	
	$listfilepath = "c:\refsc\in.txt"
	#dump files in in.txt e.g
	$dir = "e:\"
	$files = Get-ChildItem -Path $dir -Recurse -Depth 10 -file -Include "*.vbk","*.vib","*.vrb" | % { $_.FullName }
	$files | set-content $listfilepath

	$outputpath = "c:\refsc\out.xml"

	$x = Start-Process -FilePath $exe -ArgumentList @("-x","-i",$listfilepath,"-o",$outputpath) -Wait -PassThru
	$result = [xml](Get-Content $outputpath)

	write-host ("Total Savings {0}" -f $result.result.totalshare.bytes)
						
						*/

#include "stdafx.h"
#include "windows.h"
#include "Shlwapi.h"
#include "blockstat.h"
#pragma comment(lib, "Shlwapi.lib")

//very big size in characters
#define SUPERMAXPATH 4096
#define ERRORWIDTH 4096
//max amount of files blockstat will compare
//
//#define SOMETHINGFISHY
#ifdef SOMETHINGFISHY
	#define MAXCOMPAREFILES 16384
#else
	#define MAXCOMPAREFILES 1024
#endif

#include <iostream>

//Don't have mem will use a uint8_t for referencing counting. This will slash memory usage in half
//However, in this case a block can not be shared more then 255 time because it will overflow to 0 again and give false results
//with uint16 64k shares are possible, which should not be reached so fast
//#define DONTHAVEANYMEM


#ifdef DONTHAVEANYMEM
typedef uint8_t ShareMemCounterInt;
#else
typedef uint16_t ShareMemCounterInt;
#endif


//generic stacking function for strings
//makes an array of strings that will automatically resize when using addStrStack
//c = how many actually used
//l = provisioned space
//ss = current array
typedef struct _stringstack {
	int c;
	int l;
	wchar_t** ss = NULL;
} StringStack;
//free the lines itself + the stack
void freeStringStack(StringStack * ps) {
	for (int i = 0; i < ps->c; i++) {
		free(ps->ss[i]);
	}
	free(ps->ss);
}
//make a new empty stack
StringStack* newStringStack() {
	StringStack* stack = (StringStack*)malloc(sizeof(StringStack));
	stack->c = 0;
	stack->l = 0;
	stack->ss = nullptr;
	return stack;
}
//if stack is big enough, add the pointer
//if not, resize by 4x the current size
void addStrStack(StringStack* ssp, wchar_t * pushstr) {
	if (ssp->c < ssp->l) {
		//printf("%d %d\n", ssp->c, ssp->l);
		ssp->ss[ssp->c] = pushstr;
		(ssp->c)++;
	}
	else {
		//new length
		int newlength = 5;
		if (ssp->l > 0) { newlength = ssp->l * 4; }

		//allocate new warray
		wchar_t** newarr = (wchar_t**)malloc(sizeof(wchar_t*) * newlength);

		//copy pointers
		for (int i = 0; i < ssp->l; i++) {
			(newarr)[i] = ssp->ss[i];
		}
		//add the new string
		(newarr)[ssp->c] = pushstr;

		//update the count (used counter)
		(ssp->c)++;
		//update to the new length
		ssp->l = newlength;
		//free the old array
		free(ssp->ss);
		//assign the newly created array with pointers
		ssp->ss = newarr;

		//wprintf(L"resized");
	}
}

//error handling
//just a generic function to print out the last error in a readable format
void printLastError(LPCWSTR errdetails) {
	wchar_t errorbuffer[ERRORWIDTH];
	DWORD code = GetLastError();
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorbuffer, ERRORWIDTH, NULL);
	wprintf(L"%ls : %ld %ls\n", errdetails, code, errorbuffer);
}


//add the latest error to the stack
void addStringStackError(StringStack * ps, LPCWSTR errdetails) {
	wchar_t errorbuffer[ERRORWIDTH];
	DWORD code = GetLastError();
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorbuffer, ERRORWIDTH, NULL);

	//20 should be enough to hold " : "+num+eol
	int allocsize = (wcslen(errorbuffer) + wcslen(errdetails) + 20);
	wchar_t * endresult = (wchar_t*)(malloc(allocsize* sizeof(wchar_t)));
	swprintf_s(endresult,allocsize, L"%ls : %ld %ls\n", errdetails, code, errorbuffer);

	addStrStack(ps, endresult);
}


//structs for vcnquering
//should already be defined in the headers. Just manually overwritten to see if we can figure out why querying multiple extents doesnt work
typedef struct _VCNLCNMAP {
	LARGE_INTEGER NextVcn;
	LARGE_INTEGER Lcn;
} VCNLCNMAP, *PVCNLCNMAP;

typedef struct _VINFO {
	DWORD ClusterSize;
	//DWORD Clusters;
	ULONGLONG Clusters;
	wchar_t Volume[SUPERMAXPATH];
} VINFO;

//compare structs
//might be good to analyse vcnnums/compare function for more info on how this is used
typedef struct _shareline {
	LONGLONG savingsbytes;
	LONGLONG savingsmb;
	int shareratio;
} ShareLine;

typedef struct _compareresult {
	StringStack * files;
	StringStack * errors;
	ShareLine* sharelines;
	int sharelinesc;
	LONGLONG savings;
	LONGLONG fragments;
	VINFO* gvinfo = NULL;
	
} CompareResult;

//single structs
//might be good to analyse vcnnums function for more info on how this is used
typedef struct _vcnres {
	LONGLONG startvcn;
	LONGLONG lcn;
	LONGLONG sizepart;
	LONGLONG totsize; //so far in the file
} VCNRes;

typedef struct _vcnstack {
	long provisioned;
	long used;
	VCNRes** vs;
} VCNStack;

//creating a vcn stack
VCNStack* newVCNStack() {
	VCNStack * v = (VCNStack*)(malloc(sizeof(VCNStack)));
	v->provisioned = 20;
	v->used = 0;
	v->vs = (VCNRes**)(malloc(sizeof(VCNRes*) * 20));
	return v;
}
//add a vcn to the result when querying one single file (singleresult)
//add's if the current array is big enough
//otherwise make an array 4* the current size
//copies the pointers
//add the result
//updates the struct and remove the old stack
void addVCNStack(VCNStack *stack, VCNRes * result) {
	if (stack->used < stack->provisioned) {
		(stack->vs)[stack->used] = result;
		stack->used++;
	}
	else {

		long newsize = stack->provisioned * 4;

		//wprintf(L"resizing %lld +",newsize);

		VCNRes ** newstack = (VCNRes**)(malloc(sizeof(VCNRes*) * newsize));

		for (long i = 0; i < stack->used; i++) {
			newstack[i] = stack->vs[i];
		}
		newstack[stack->used] = result;

		VCNRes ** oldstack = stack->vs;
		stack->provisioned = newsize;
		stack->used++;
		stack->vs = newstack;

		free(oldstack);
	}
}
//result when querying one file
typedef struct _singleResult {
	wchar_t * file;
	StringStack * errors;
	VCNStack * vcnstack;
	VINFO* gvinfo = NULL;
} SingleResult;



//generic option struct
typedef struct _Blockstatflags {
	bool xmlout;
	FILE* printer;
	bool printerisfile;
	bool verbose;
} Blockstatflags;

//generic function to get volume the volume info we need
bool GetVolInfo(wchar_t * pfname, VINFO * vinfo) {
	bool success = false;

	if (GetVolumePathName(pfname, vinfo->Volume, SUPERMAXPATH)) {
		DWORD SectorsPerCluster;
		DWORD BytesPerSector;
		DWORD NumberOfFreeClusters;
		DWORD TotalNumberOfClusters;

		if (GetDiskFreeSpace(vinfo->Volume, &SectorsPerCluster, &BytesPerSector, &NumberOfFreeClusters, &TotalNumberOfClusters)) {
			//vinfo->Clusters = TotalNumberOfClusters;
			vinfo->ClusterSize = SectorsPerCluster*BytesPerSector;

			//very big volumes don't like the 32 bit integer 
			//16TB at 4KB cluster size maxes out TotalNumberOfClusters
			
			ULARGE_INTEGER TotalNumberOfBytes;
			if (GetDiskFreeSpaceEx(vinfo->Volume, NULL, &TotalNumberOfBytes, NULL)) {
				vinfo->Clusters = (TotalNumberOfBytes.QuadPart / vinfo->ClusterSize);

				
				success = true;
			}
		}
	}
	return success;
}

//adding errors to the result for printing later
void resulterradd(SingleResult * singleresult, CompareResult * compareresult,LPCWSTR errprefix) {
	if (singleresult != NULL) {
		addStringStackError(singleresult->errors, errprefix);
	}
	else if (compareresult != NULL) {
		addStringStackError(compareresult->errors, errprefix);
	}
	else {
		wprintf(L"Error printing fails, should not happen, predumping to console\n");
		printLastError(errprefix);
	}
}

//the heart of the app

bool vcnnums(HANDLE * psrchandle, VINFO* vinfo, ShareMemCounterInt * refmap, LONGLONG refmapsz, bool singlefiledump, SingleResult * singleresult,CompareResult * compareresult, Blockstatflags * bsf) {

	//need to have  a file handle open to the file
	HANDLE fhandle = *psrchandle;
	bool success = FALSE;

	//what is the clustersize
	LONGLONG clustersize = vinfo->ClusterSize;

	


	//VCN -> virtual cluster number 
	//	This reference the cluster number in the file itself. E.g 0 points to the beginning of the file, the last (VCN+The size of the extent)*clustersize should be the size of the file
	//LCN -> logical cluster number
	//	Where is the block located on the volume. It makes the mapping from VCN to volume. While the first VCN for every file is 0, the first LCN will of course not be because that would mean that every file starts at location zero on the filesystem

	//to query the vcn number, we have to pass the previous result last vcn number
	//at the start, we just pass 0 to say we are starting at the verry beginning
	LONGLONG startvcn = 0;

	//we need this to hold the ref to the very first vcn
	STARTING_VCN_INPUT_BUFFER StartingPointInputBuffer = { 0 };
	StartingPointInputBuffer.StartingVcn.QuadPart = startvcn;

	//Normally you should be able to query multiple VCN + Extentsize ( reference of a block/fragment) at once but this give bad result
	//querying them one by one seems to work
	INT extents = 4096;
	extents = 1;

	//2*LI = StartVCN + ExtCount | 2*LI*extents = (NextVcn+Lcn)*extents
	//Per "extent" we need 2* the result of large integer
	//on top of that, we will store the the startvcn
	INT iExtentsBufferSize = (sizeof(LARGE_INTEGER)*2)+((sizeof(LARGE_INTEGER) * 2)*extents);

	//we allocate space for this in memory using the previously calculated size
	//this is overcomplicated code since we query only one extent at the time but just if ever we can figure out why the extents 4096 does not work
	PRETRIEVAL_POINTERS_BUFFER lpRetrievalPointersBuffer = (PRETRIEVAL_POINTERS_BUFFER)malloc(iExtentsBufferSize);

	//how much data is return by the code
	//obligatory field for the call pointer retrieval call
	DWORD dwBytesReturned;

	
	//while contstatus is 0, we keep quering (means we have more data)
	int contstatus = 0;

	//how much clusters did we count so far
	LONGLONG clusterstotal = 0;

	//how much fragments / vcn / extents did we query so far
	LONGLONG dumpedextents = 0;

	while (contstatus == 0) {
		//on the file execute get pointers. Watchout they do not refer to the physical volume but rather to the logical volume
		BOOL s = DeviceIoControl(fhandle,FSCTL_GET_RETRIEVAL_POINTERS,&StartingPointInputBuffer,sizeof(STARTING_VCN_INPUT_BUFFER),lpRetrievalPointersBuffer,iExtentsBufferSize,&dwBytesReturned,NULL);

		//if sucess = true, there is no error. It means all the pointers retrieval fitted into the buffer
		//basically it means we are at the end of the file
		if (s) { contstatus = 1; success = true; }
		//if we have success = false, that doesn't mean there is a real issue. In most cases, it just means the data did not fit completely in the the buffer. Means we need to requery
		//if however the error does not equal ERROR_MORE_DATA, something else went wrong, and we stop the process
		else {
			DWORD error = GetLastError();

			if (error == ERROR_HANDLE_EOF) {
				if (bsf->verbose) { wprintf(L"VERBOSE: is a small file?\n"); }
				contstatus = 1;
				success = true;
			}
			else if (error != ERROR_MORE_DATA) {
				contstatus = 2;
				//printLastError(L"Something went wrong with device io control");
				resulterradd(singleresult, compareresult, L"Something went wrong with device io control");
			}
			
		}
		
		//derefence for easy access (and shorter name)
		RETRIEVAL_POINTERS_BUFFER rpb = *lpRetrievalPointersBuffer;

		//what is the startvcn
		LONGLONG startvcn = rpb.StartingVcn.QuadPart;

		//convert the extent array from a pointer to an array
		PVCNLCNMAP extents = (PVCNLCNMAP)&rpb.Extents;

		//count the amount of extents, then go over every extent
		//although rpb.ExtentCount will always be 1 with extents set to 1, this code should still support it
		dumpedextents += rpb.ExtentCount;
		for (DWORD ec = 0; ec < (rpb.ExtentCount); ec++) {
			//checking the x extent
			VCNLCNMAP extent = extents[ec];
			//location on disk
			LARGE_INTEGER lcn = extent.Lcn;
			//what is the nextvcn
			LARGE_INTEGER nextvcn = extent.NextVcn;

			//the size of this extent is the (nextvcn it's address - the current vcn/startvcn)
			//startvcn needs to be updated at the end of the for loop
			//this tells basically how big the current extent is in clusters
			LONGLONG extclusters = (nextvcn.QuadPart - startvcn);

			//count the total amount of clusters
			clusterstotal += extclusters ;

			if (!singlefiledump) {
				//if we are comparing (not a single file), we update the refmap
				//vcn is only offset + size
				//so we need to flag for every cluster in this part
				/*
					eg		[x][y][z]
					cluster	 2  3  4 

					vcn will say, start at cluster 2 and is 3 clusters big (lcnend)
					so for 2,3 & 4 (which hold x, y,z) we need to increment the share counter for the cluster

					A filesystem will always try to make a  bigger extent so that the data can be accessed more sequentially
				*/
				LONGLONG lcnend = (lcn.QuadPart + extclusters);

				if (lcnend <= refmapsz) {
					for (LONGLONG cl = lcn.QuadPart; cl < lcnend; cl++) {
						refmap[cl]++;
					}

				}
				else {
					wprintf(L"REFMAP NOT BIG ENOUGH (SHOULD NOT HAPPEN)\n");
					wprintf(L"LCN END (end of extent) was %lld vs size of map %lld\n", lcnend, refmapsz);
					wprintf(L"CLUSTERS %lld CSIZE %ld\n",vinfo->Clusters,vinfo->ClusterSize);
				}
				//increment the fragments result so we can see how fragmented a file is
				compareresult->fragments++;
			}
			else {

				//if it is a single file, we just make a reference
				VCNRes * vrs = (VCNRes*)(malloc(sizeof(VCNRes)));
				vrs->startvcn = startvcn;
				vrs->lcn = lcn.QuadPart;
				vrs->sizepart = (extclusters*clustersize);
				//tot size is not the total size of the file itself. It should tell use how much data is already "processed"
				vrs->totsize = (clusterstotal*clustersize);

				addVCNStack(singleresult->vcnstack, vrs);
			}

			
			//the new startvcn (cluster number) is set to nextvcn, so that we can do correct calculation on the size of the of the extent
			startvcn = nextvcn.QuadPart;
			
		}
		//set the startingvcn to the nextvcn of the last extent so we can query more pointers
		StartingPointInputBuffer.StartingVcn.QuadPart = startvcn;
	}

	free(lpRetrievalPointersBuffer);
	return success;
}

/*
1 FILE DUMP FUNCTIONS

Shows where the file is logically located on the file system
*/

//should be fairly easy to understand
//just prints out the info from the structs in human readable format
void printsingle(Blockstatflags* bsf, SingleResult * psr) {
	fwprintf(bsf->printer, L"Single Mode\n");
	fwprintf(bsf->printer, L"Fsinfo %ls clustersize %lld clusters %lld\n", psr->gvinfo->Volume, psr->gvinfo->ClusterSize, psr->gvinfo->Clusters);
	fwprintf(bsf->printer, L"File: %ls\n",psr->file);
	for (int i = 0; i < psr->vcnstack->used; i++) {
		VCNRes *vrs = psr->vcnstack->vs[i];
		fwprintf(bsf->printer, L"%20lld LCN %20lld SZ %20lld TSZ %20lld \n", vrs->startvcn,vrs->lcn,vrs->sizepart,vrs->totsize);
	}
	fwprintf(bsf->printer, L"Total Extents : %lld",psr->vcnstack->used);
}
//should be fairly easy to understand
//just prints out the info from the structs in xml

void xmlprintsingle(Blockstatflags* bsf, SingleResult * psr) {
	fwprintf(bsf->printer, L"<result type='single'>\n");

	fwprintf(bsf->printer, L" <fsinfo volume='%ls' clustersize='%lld' clusters='%lld'/>\n", psr->gvinfo->Volume, psr->gvinfo->ClusterSize, psr->gvinfo->Clusters);
	fwprintf(bsf->printer, L" <files>\n");
	fwprintf(bsf->printer, L"\t<file>%ls</file>\n", psr->file);
	fwprintf(bsf->printer, L" </files>\n");


	if (psr->errors->c > 0) {
		fwprintf(bsf->printer, L" <errors>\n");
		for (int i = 0; i < psr->errors->c; i++) {
			fwprintf(bsf->printer, L"\t<error>%ls</error>\n", psr->errors->ss[i]);
		}
		fwprintf(bsf->printer, L" </errors>\n");
	}

	fwprintf(bsf->printer, L" <vcns>\n");
	for (int i = 0; i < psr->vcnstack->used; i++) {
		VCNRes *vrs = psr->vcnstack->vs[i];
		fwprintf(bsf->printer, L"\t<vcn start='%lld' lcn='%lld' sz='%lld' totalsz='%lld' />\n", vrs->startvcn, vrs->lcn, vrs->sizepart, vrs->totsize);
	}
	fwprintf(bsf->printer, L" </vcns>\n");
	fwprintf(bsf->printer, L" <totalextents>%lld</totalextents>\n", psr->vcnstack->used);

	fwprintf(bsf->printer, L"</result>\n");
}

//function for one file processing, will call xmlprintsingle or printsingle depending on the user request (-x vs nothing specified)
int dumpfile(Blockstatflags* bsf,wchar_t* src) {
	int retvalue = 0;

	//result for printing
	SingleResult sr;

	//VCN = virtual cluster number
	//what is the offset and how long is it
	sr.vcnstack = newVCNStack();
	sr.errors = newStringStack();
	sr.file = src;


	//get the volume info struct in place (used to query volume size, cluster size, etc.)
	VINFO* vinfo = (VINFO*)malloc(sizeof(VINFO));
	(vinfo->Volume)[0] = 0;
	sr.gvinfo = vinfo;

	//if the file exists, we can do something
	//should already be checked by main
	if (PathFileExists(src)) {


		//get the volume info by referencing the file
		if (GetVolInfo(src, vinfo)) {
			
			//if we can get the vol info, we try to open the file in read/shared modus
			HANDLE srchandle = CreateFile(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (srchandle != INVALID_HANDLE_VALUE) {

				//if we can open the file, we can query the the cluster information
				//vcnnums will update the singleresult so it can be used by the printing functions
				if (!vcnnums(&srchandle, vinfo, NULL, 0, true,&sr,NULL,bsf)) {
					retvalue = 4;
					
					addStringStackError(sr.errors, L"No success vcnnums");
				}


				CloseHandle(srchandle);
			}
			else {
				retvalue = 3;
				addStringStackError(sr.errors, L"Error opening file handle (might be in use?)");
				
			}

		}
		else {
			retvalue = 4;
			
			addStringStackError(sr.errors, L"Error getting vol info for file");

		}
		

	} else {
		addStrStack(sr.errors, L"File does not exists");
	}
	//if xml, use the corresponding function
	if (bsf->xmlout) {
		xmlprintsingle(bsf, &sr);
	}
	else {
		printsingle(bsf, &sr);
	}
	//cleanup some stuff
	free(vinfo);

	for (int i=0; i < sr.vcnstack->used; i++) {
		free(sr.vcnstack->vs[i]);
	}
	free(sr.vcnstack->vs);

	free(sr.vcnstack);

	free(sr.errors);
	
	return retvalue;
}




/*

COMPARING FUNCTIONS

*/
//should be fairly easy to understand
//just prints out the info from the structs in human readable format
void printcompare(Blockstatflags* bsf,CompareResult * compareresult) {
	fwprintf(bsf->printer,L"Comparing Mode\n");
	fwprintf(bsf->printer, L"Fsinfo %ls clustersize %lld clusters %lld\n", compareresult->gvinfo->Volume, compareresult->gvinfo->ClusterSize, compareresult->gvinfo->Clusters);
	fwprintf(bsf->printer, L"Files:\n");
	for (int i = 0; i < compareresult->files->c; i++) {
		fwprintf(bsf->printer, L"\t- %ls\n", compareresult->files->ss[i]);
	}
	fwprintf(bsf->printer, L"\n");
	

	if (compareresult->errors->c > 0) {
		fwprintf(bsf->printer, L"Errors:\n");
		for (int i = 0; i < compareresult->errors->c; i++) {
			fwprintf(bsf->printer, L"\t-%ls\n", compareresult->errors->ss[i]);
		}
		fwprintf(bsf->printer, L"\n");
	}

	fwprintf(bsf->printer, L"Sharing:\n");
	for (int i = 0; i < compareresult->sharelinesc; i++) {
		fwprintf(bsf->printer, L"\t- %ld x \t %lld bytes %lld mb\n", compareresult->sharelines[i].shareratio, compareresult->sharelines[i].savingsbytes, compareresult->sharelines[i].savingsmb);
	}

	fwprintf(bsf->printer, L"\n\nTotal Savings %lld (%lld mb)\n", compareresult->savings, ((compareresult->savings) / 1024 / 1024));
	fwprintf(bsf->printer, L"Total Fragments Over All Files %lld\n", compareresult->fragments);

}

//should be fairly easy to understand
//just prints out the info from the structs in xml
void xmlprintcompare(Blockstatflags* bsf,CompareResult * compareresult) {
	
	
	fwprintf(bsf->printer,L"<result type='compare'>\n");
	
	fwprintf(bsf->printer, L" <fsinfo volume='%ls' clustersize='%lld' clusters='%lld'/>\n", compareresult->gvinfo->Volume, compareresult->gvinfo->ClusterSize, compareresult->gvinfo->Clusters);
	fwprintf(bsf->printer, L" <files>\n");
	for (int i=0; i < compareresult->files->c; i++) {
		fwprintf(bsf->printer, L"\t<file>%ls</file>\n", compareresult->files->ss[i]);
	}
	fwprintf(bsf->printer, L" </files>\n");


	if (compareresult->errors->c > 0) {
		fwprintf(bsf->printer, L" <errors>\n");
		for (int i = 0; i < compareresult->errors->c; i++) {
			fwprintf(bsf->printer, L"\t<error>%ls</error>\n", compareresult->errors->ss[i]);
		}
		fwprintf(bsf->printer, L" </errors>\n");
	}

	fwprintf(bsf->printer, L" <shares>\n");
	for (int i = 0; i < compareresult->sharelinesc; i++) {
		fwprintf(bsf->printer, L"\t<share ratio='%ld' bytes='%lld' mb='%lld'/>\n", compareresult->sharelines[i].shareratio, compareresult->sharelines[i].savingsbytes, compareresult->sharelines[i].savingsmb);
	}
	fwprintf(bsf->printer, L" </shares>\n");
	fwprintf(bsf->printer, L" <totalshare bytes='%lld' mb='%lld'/>\n",compareresult->savings,((compareresult->savings)/1024/1024));
	fwprintf(bsf->printer, L" <fragments count='%lld'/>\n", compareresult->fragments);
	fwprintf(bsf->printer, L"</result>\n");
}

//compare files will do the comparisson and built a CompareResult
//this can be passed to xmlprint or print depending if the output should be xml or not

int comparefiles(Blockstatflags* bsf,wchar_t* filesa[],int filesc) {
	//retvalue = 0 return value 0 means all was ok
	int retvalue = 0;

	//how many goodfiles do we have
	//for block comparisson, they need to be on the same volume
	//this is checked against the very first file
	int goodfiles = 0;
	wchar_t ** files = (wchar_t**)malloc(sizeof(wchar_t*) * MAXCOMPAREFILES);

	//VINFO struct is requried to check the volume name, cluster size &volume size for a file
	VINFO* gvinfo = (VINFO*)malloc(sizeof(VINFO));
	(gvinfo->Volume)[0] = 0;

	//init the compareresult
	CompareResult compareresult = { };
	compareresult.errors = newStringStack();
	compareresult.files = newStringStack();
	//sharelines is a special struct that tells how many mb is x amount shared
	compareresult.sharelines = nullptr;
	compareresult.savings = 0;
	compareresult.fragments = 0;



	if (bsf->verbose) { wprintf(L"VERBOSE: Checking if files are on the same volume\n"); }
	

	//make sure all files are on the same volume as first path
	for (int f = 0; f < filesc && f < MAXCOMPAREFILES; f++) {
		wchar_t * src = filesa[f];

		//if path exists (should already be done by main but just to make sure)
		if (PathFileExists(src)) {

			VINFO* vinfo = (VINFO*)malloc(sizeof(VINFO));
			(vinfo->Volume)[0] = 0;

			//get volume info for a file
			if (GetVolInfo(src, vinfo)) {
				//if we didn't check any files or the volume is the same for the next file, we add it to the array of goodfiles
				if (goodfiles == 0 || _wcsicmp(gvinfo->Volume, vinfo->Volume) == 0) {
					//first file is always a goodfile, we use it as the baseline for the volume (clustersize etc.)
					//next files will be checked against it to see if the files are on the same path
					if (goodfiles == 0) {
						gvinfo = vinfo;
						compareresult.gvinfo = gvinfo;
					}
					//we don't need to keep the same info for each file
					else { free(vinfo); }

					//adding file to the array of good files and to the output stack
					//could be optimized by using the stringstack of compareresult
					files[goodfiles] = src;
					addStrStack(compareresult.files, src);
					goodfiles++;

					if (bsf->verbose) { wprintf(L"VERBOSE: File %ls is good\n",src); }
				}
				else {
					//if file 2 and subsequent files are not on the same vol, we can not look for shared clusters because there is 0% chance of finding any
					addStrStack(compareresult.errors, L"Not on same vol");
				}
			}
			else {
				//could not query vol info for a file
				addStringStackError(compareresult.errors, L"Error getting file vol info");

			}
		}
		else {
			//should not happen because already checked by main
			addStrStack(compareresult.errors, L"File does not exist");
		}
	}

	//if more then 1 goodfile (more then 1 file on the same vol), we can compare
	if (goodfiles > 1) {
		if (bsf->verbose) { wprintf(L"VERBOSE: Got enough files, starting to compare\n"); }
		//making a very inefficient int array. The size of the array equals the amount of clusters on the volume itself. 
		//this make it so that the bigger the volume is, the more memory the program uses
		//there is thus no link with the filesize itself
		//everytime a cluster is found, the corresponding int is incremented with 1 does indicating how much the block is used


		LONGLONG refmapsz = sizeof(ShareMemCounterInt)*gvinfo->Clusters;
		ShareMemCounterInt* refmap = (ShareMemCounterInt*)malloc(refmapsz);


		
		//zeroing the array
		for (LONGLONG r = 0; r < gvinfo->Clusters; r++) {
			refmap[r] = 0;
		}

		
		//for every file, open it and check the used clusters
		for (int f = 0; f < goodfiles; f++) {
			//open file in read (shared) mode
			HANDLE srchandle = CreateFile(files[f], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);


			//if we can open the file, all is good
			if (srchandle != INVALID_HANDLE_VALUE) {
				if (bsf->verbose) { wprintf(L"VERBOSE: Comparing %ls\n", files[f]); }

				//call the vcn num function who updates the refmap with the amount of clusters
				if (!vcnnums(&srchandle, gvinfo, refmap, refmapsz, false,NULL,&compareresult,bsf)) {
					retvalue = 4;
					
					addStringStackError(compareresult.errors, L"No success vcnnums on file");
					
				}
				//closing
				CloseHandle(srchandle);
			}
			else {
				retvalue = 3;
				addStringStackError(compareresult.errors, L"Error opening file (in use?)");
			}
		}

		/*
			we now have a map with has the share ratio per cluster
			lets make a new shared array that counts per ratio the amount of bytes share
			Imagine

			refsmap => [ 1 ][ 1 ][ 1 ][ 3 ][ 1 ][ 1 ][ 2 ][ 3 ]
			this represents 8 cluster. Of those clusters:
			5 are shared only 1 time (they are actually unique)
			1 is shared 2 times (means that savings is (2-1)*1 = 1
			2 are shared 3 times (means that savings is (3-1)*2 = 4
			shared => [5] [1] [2]

		*/

		if (bsf->verbose) { wprintf(L"VERBOSE: Files compared, building up share array for final stats\n"); }
		LONGLONG shared[MAXCOMPAREFILES];
		for (int i = 0; i < MAXCOMPAREFILES; i++) {
			shared[i] = 0;
		}

		//what is the highest share ratio (topshare)
		int topshare = 1;

		//making the array as show above
		for (LONGLONG r = 0; r < gvinfo->Clusters; r++) {
			//if a cluster was flagged (used by one of the files)
			if (refmap[r] != 0) {
				//if the sharing ratio is smaller then the amount of files
				//theoretically, this might be possible if one files is refering the same data block but would be strange
				//could break the array because it is based on MAXCOMPAREFILEs
				//in this case the result should not be correct
				if (refmap[r] < MAXCOMPAREFILES) {
					//updating the share map
					shared[refmap[r]]++;
					if (topshare < refmap[r]) {
						topshare = refmap[r];
					}
				}
				else {
					addStrStack(compareresult.errors, L"More shared then files, seems impossible?");
				}
			}
		}
		if (bsf->verbose) { wprintf(L"VERBOSE: Building up output, get ready to process\n"); }
		//theoretically the share ratio map is enough to pass the info
		//however, this does the precalculations so that the print function do not have to implement it individually (e.g sharelines)
		LONGLONG savings = 0;
		//allocating at the highest share ratio. Might be too much if for example all blocks are share 2x but no 1x (2 duplicate files)
		compareresult.sharelines = (ShareLine*)malloc(sizeof(ShareLine)*(topshare));
		compareresult.sharelinesc = 0;

		for (int i = 0; i < MAXCOMPAREFILES; i++) {
			//if shared ratio is bigger then 0
			if (shared[i] > 0) {
				//how much data is really shared (ratio multiplied by clustersize
				LONGLONG bytesshr = (shared[i] * gvinfo->ClusterSize);

				compareresult.sharelines[compareresult.sharelinesc].savingsbytes = bytesshr;
				//convert to MB
				compareresult.sharelines[compareresult.sharelinesc].savingsmb = bytesshr / 1024 / 1024;
				//ratio is independently given
				//cannot use array index as 1x for example will not occure
				//this is easier on the post/printing side
				compareresult.sharelines[compareresult.sharelinesc].shareratio = i;
				compareresult.sharelinesc++;

				//how much is saved
				//if a data is shared 1 time, it means it is uniquely used thus there is no gain
				//if a data is shared 2 time, it needs to be stored 1 time, and is reused 1 time
				//if a data is shared 3 time, it needs to be stored 1 time, and is reused 2 time
				//etc.. (i-1) reuse
				if (i > 1) {
					savings += (i - 1)*bytesshr;
				}
			}
		}
		compareresult.savings = savings;

		



		free(refmap);
	}
	else {
		retvalue = 2;
		addStrStack(compareresult.errors, L"Make sure that files are on the same volume as the first file");
	}

	//depending on the output, printing
	if (bsf->xmlout) {
		xmlprintcompare(bsf, &compareresult);
	}
	else {
		printcompare(bsf, &compareresult);
	}
	if (bsf->verbose) { wprintf(L"VERBOSE: Done"); }
	free(files);
	free(compareresult.files->ss);
	
	free(compareresult.files);
	free(compareresult.errors);
	
	if (compareresult.sharelines != NULL) {
		free(compareresult.sharelines);
	}

	free(gvinfo);

	return retvalue;
}
bool isdir(bool * isdir, wchar_t * dir) {
	WIN32_FIND_DATA ffd;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	bool ok = true;
	(*isdir) = false;

	int len = wcslen(dir);

	hFind = FindFirstFile(dir, &ffd);
	if (INVALID_HANDLE_VALUE != hFind) {
		(*isdir) = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY );
		FindClose(hFind);
	}
	else if (len > 1 && len < 4 && dir[1] == L':' && PathFileExists(dir)) {
			(*isdir) = true;
	} 
	else {
		ok = false;
	}
	return ok;
}

void recursiveadddir(wchar_t* basedir, int* count, wchar_t** files) {
	WIN32_FIND_DATA ffd;
	HANDLE hFind = INVALID_HANDLE_VALUE;
	wchar_t * qdir = (wchar_t*)(malloc(sizeof(wchar_t)*SUPERMAXPATH));

	if ((*count) < MAXCOMPAREFILES && wcslen(basedir)+3 < SUPERMAXPATH) {
		wcscat_s(basedir, SUPERMAXPATH, L"\\");
		wcscpy_s(qdir, SUPERMAXPATH, basedir);
		wcscat_s(qdir, SUPERMAXPATH,L"*");
		hFind = FindFirstFile(qdir, &ffd);

		
		if (INVALID_HANDLE_VALUE != hFind)
		{
			do {
				wchar_t * nextpath = (wchar_t*)(malloc(sizeof(wchar_t)*SUPERMAXPATH));
				nextpath[0] = L'\0';
				wcscpy_s(nextpath, SUPERMAXPATH, basedir);
				wcscat_s(nextpath, SUPERMAXPATH, ffd.cFileName);

				if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					if (!(wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)) {
						//wprintf(L"-%ls\n", nextpath);
						recursiveadddir(nextpath, count, files);
						free(nextpath);
					}
				}
				else {
					if (PathFileExists(nextpath)) {
						files[(*count)] = nextpath;
						(*count) = (*count) + 1;
					}
					//wprintf(L"%5ld %ls\n",(*count),nextpath);
				}
			} while ((*count) < MAXCOMPAREFILES && FindNextFile(hFind, &ffd) != 0);
			FindClose(hFind);
		}
		//else { wprintf(L"invalid handle %ls\n",qdir); }
	}
	//else { wprintf(L"path too big\n"); }

	free(qdir);
	
}

int main(int argc, char* argv[])
{
	int retvalue = 0;

	

	//General options to pass through to all the functions
	//xmlout -> should we output human readable vs xml
	//printer -> define the stream where to write to. Default stdout is screen (printerisfile needs to be set to true if not stdout so that the file is flushed and closed)
	Blockstatflags * bsf = (Blockstatflags*)(malloc(sizeof(Blockstatflags)));
	bsf->xmlout = false;
	bsf->printer = stdout;
	bsf->printerisfile = false;
	bsf->verbose = false;
	
	
	//Read from file defines an empty buffer to write to if the -i parameter is given (e.g a file that contains a filename per line)
	char* readfromfile = (char*)malloc(sizeof(char)*SUPERMAXPATH);
	readfromfile[0] = 0;
	
	//An array of file names used to compare. If there is only one file, there won't be any comparission, just a dump of the extents
	wchar_t** files = (wchar_t**)malloc(sizeof(wchar_t*)*MAXCOMPAREFILES);
	int filesc = 0;

	//process all the arguments given. Argument without dash is considered a file if the previous arg was not an arg specifier
	for (int i = 1; i < argc && filesc < MAXCOMPAREFILES; i++) {
		//check if the file is an argument specifier
		if (strlen(argv[i]) > 1 && argv[i][0] == '-') {
			switch (argv[i][1]) {
			//-x means we need to output xml
			case 's':
				printf("\nHidden option: sizeof refmap is %d",sizeof(ShareMemCounterInt));
				printf("\nHidden option: sizeof maxcomparefiles is %d\n", MAXCOMPAREFILES);

				if ((i + 1) < argc) {
					wchar_t * filealloc = (wchar_t*)malloc(sizeof(wchar_t)*SUPERMAXPATH); filealloc[0] = 0;
					size_t conv = { 0 };
					//copy compare
					mbstowcs_s(&conv, filealloc, SUPERMAXPATH, argv[i+1], strlen(argv[i+1]));

					//if the file exists, add it to the file stack for comparissoon
					if (PathFileExists(filealloc)) {
						VINFO* vinfo = (VINFO*)malloc(sizeof(VINFO));
						(vinfo->Volume)[0] = 0;
						if (GetVolInfo(filealloc, vinfo)) {
							wprintf(L"%lld * %lld\n", vinfo->Clusters, vinfo->ClusterSize);
							wprintf(L"Volsize: %lld GB", ((vinfo->Clusters* vinfo->ClusterSize)/1024/1024/1024));
						}
						free(vinfo);
					}
					free(filealloc);

					
				}

				return 2001;
				break;
			case 'x':
				bsf->xmlout = true;
				break;
			//-o we need to output to a file
			case 'o':
				//need at least an extra argument after -o that specifies the file
				if ((i + 1) < argc) {

					//allocating the file
					FILE* ff = (FILE*)(malloc(sizeof(FILE)));

					//open the file for writing
					if (!fopen_s(&ff, argv[i + 1], "w+")) {
						bsf->printerisfile = true;
						bsf->printer = ff;
					}
					else {
						//if we can not open the file, instantly closing
						wprintf(L"DIE: UNABLE TO OPEN FILE\n");
						return 1001;
					}
					
					i++;
				}
				break;
			//-i, read files names to compare from a file. This is handy if you want to wrap in powershell
			//no processing is actually done here
			case 'i':
				if ((i + 1) < argc) {
					strcpy_s(readfromfile, SUPERMAXPATH*(sizeof(char)), argv[(i + 1)]);
					i++;
				}
				break;
			case 'm':
				if ((i + 1) < argc) {
					i++;

					WIN32_FIND_DATA ffd;
					HANDLE hFind = INVALID_HANDLE_VALUE;

					wchar_t * filealloc = (wchar_t*)malloc(sizeof(wchar_t)*SUPERMAXPATH); filealloc[0] = L'\0';
					size_t conv = { 0 };
					//copy compare
					mbstowcs_s(&conv, filealloc, SUPERMAXPATH, argv[i], strlen(argv[i]));

					hFind = FindFirstFile(filealloc, &ffd);
					if (INVALID_HANDLE_VALUE != hFind)
					{
						size_t ll = wcslen(filealloc) - 1;
						while (ll > 0 && filealloc[ll] != L'\\') {
							filealloc[ll--] = L'\0';
						}

						
						do
						{
							if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
							{
								wchar_t *fpath = (wchar_t*)malloc(sizeof(wchar_t)*SUPERMAXPATH); fpath[0] = L'\0';
								wcscat_s(fpath, SUPERMAXPATH, filealloc);
								wcscat_s(fpath, SUPERMAXPATH, ffd.cFileName);

								//wprintf(L"%ls\n",fpath);
								if (PathFileExists(fpath)) {

									files[filesc] = fpath;
									filesc++;
								}
							}

						} while (FindNextFile(hFind, &ffd) != 0);

						FindClose(hFind);
					}
					else {
						wprintf(L"DIE: UNABLE TO OPEN DIRECTORY\n");
						return 1002;
					}
					free(filealloc);
				}
				//return 0;
				break;
			case 't':
				if ((i + 1) < argc) {
					i++;
					wchar_t * filealloc = (wchar_t*)malloc(sizeof(wchar_t)*SUPERMAXPATH); filealloc[0] = L'\0';
					size_t conv = { 0 };
					mbstowcs_s(&conv, filealloc, SUPERMAXPATH, argv[i], strlen(argv[i]));

					
					if (filealloc[wcslen(filealloc) - 1] == L'\\') {
						filealloc[wcslen(filealloc) - 1] = L'\0';
					}
					bool isdirb = false;
					if (isdir(&isdirb, filealloc) && isdirb) {
						recursiveadddir(filealloc, &filesc, files);
					}
					else {
						wprintf(L"DIE: UNABLE TO OPEN DIRECTORY OR IS NOT DIR\n");
						return 1004;
					}
				}
				//return 0;
				break;
			case 'd':
				if ((i + 1) < argc) {
					i++;

					WIN32_FIND_DATA ffd;
					HANDLE hFind = INVALID_HANDLE_VALUE;

					wchar_t * filealloc = (wchar_t*)malloc(sizeof(wchar_t)*SUPERMAXPATH); filealloc[0] = L'\0';
					size_t conv = { 0 };
					//copy compare
					mbstowcs_s(&conv, filealloc, SUPERMAXPATH, argv[i], strlen(argv[i]));
					if (filealloc[wcslen(filealloc) - 1] == L'\\') {
						wcscat_s(filealloc, SUPERMAXPATH, L"*");
					}
					else {
						wcscat_s(filealloc, SUPERMAXPATH, L"\\*");
					}
					hFind = FindFirstFile(filealloc, &ffd);
					if (INVALID_HANDLE_VALUE != hFind)
					{
						filealloc[wcslen(filealloc) - 1] = L'\0';
						do
						{
							if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
							{
								wchar_t *fpath = (wchar_t*)malloc(sizeof(wchar_t)*SUPERMAXPATH); fpath[0] = L'\0';
								wcscat_s(fpath, SUPERMAXPATH, filealloc);
								wcscat_s(fpath, SUPERMAXPATH, ffd.cFileName);

								if (PathFileExists(fpath)) {
									files[filesc] = fpath;
									filesc++;
								}
							}
							
						} while (FindNextFile(hFind, &ffd) != 0);

						FindClose(hFind);
					}
					else {
						wprintf(L"DIE: UNABLE TO OPEN DIRECTORY\n");
						return 1002;
					}
					free(filealloc);
				}
				
				break;
			//-h, we don't do anything
			case 'h':
				printf("-v be verbose during compare mode so you can track process\n");
				printf("-x dump as xml\n");
				printf("-i input file with file list in unicode (utf16 le)\n");
				printf("-o output file (utf16le)\n");
				printf("-d use directory supplied as input\n");
				printf("-t use directory supplied as input recursive\n");
				printf("-m mask e.g c:\\d\\file*.vbk\n");
				goto CLEANUP;
				break;
			case 'v':
				bsf->verbose = true;
				break;
			default:
				printf("Unknown option -%c\n\n",argv[i][1]);

				printf("-v be verbose during compare mode so you can track process\n");
				printf("-x dump as xml\n");
				printf("-i input file with file list in unicode (utf16 le)\n");
				printf("-o output file (utf16le)\n");
				printf("-d use directory supplied as input\n");
				printf("-t use directory supplied as input recursive\n");
				printf("-m mask e.g c:\\d\\file*.vbk\n");
				goto CLEANUP;
				break;
			}
		}
		else {
			//this argument has no leading dash so threathing it as a file

			//supermessy conversion from a regular c char to a windows widechar (because all win32 api calls require widechar)
			wchar_t * filealloc = (wchar_t*)malloc(sizeof(wchar_t)*SUPERMAXPATH); filealloc[0] = 0;
			size_t conv = { 0 };
			//copy compare
			mbstowcs_s(&conv, filealloc, SUPERMAXPATH, argv[i], strlen(argv[i]));

			//if the file exists, add it to the file stack for comparissoon
			if (PathFileExists(filealloc)) {
				files[filesc] = filealloc;
				filesc++;
			}
			else {
				//otherwise freeing resources
				free(filealloc);
			}
		}
	}

	//if strlen of readfromfile is bigger, it means somebody supplied a file with -i
	//each line in this file will be added as a file for comparisson
	if (strlen(readfromfile) > 0) {
		FILE *input;
		wchar_t buf[SUPERMAXPATH];
		buf[0] = 0;

		//open the file as UTF-16LE aka Unicode. This is done for easy "integration" with powershell.
		if ((fopen_s(&input,readfromfile, "r, ccs=UTF-16LE")) == NULL) {
			//while we can read a line from the file
			while (fgetws(buf, SUPERMAXPATH, input) != NULL) {
				bool nlq = false;
				//looking for end of the line char. If we find it, replace it with end of string (\0) char so the newline char is removed. winapi do not like new lines
				for (int i = 0; i < SUPERMAXPATH && !nlq; i++) {
					if (buf[i] == '\n' || buf[i] == '\r') {
						buf[i] = 0;
						nlq = true;
					}
				}
				//if the file exists copy it to another location (reusing buf in the while loop)
				if (PathFileExists(buf)) {
					int cplen = wcslen(buf)+1;
					wchar_t * pcp = (wchar_t*)malloc(sizeof(wchar_t)*cplen);
					//safe copy
					wcscpy_s(pcp, cplen, buf);

					//add to stack
					files[filesc] = pcp;
					filesc++;

				}
				else {
					wprintf(L"%ls does not exit\n", buf);
				}
				buf[0] = 0;
			}
			fclose(input);
		}
	}
	

	//check if there is content in stdin (aka somebody used the pipeline)
	//go to the end of the pipeline to tell the the size, then seek back to the beginning
	//this doesn't work at all with powershell since it sends unicode through the pipe, better use -i
	fseek(stdin, 0, SEEK_END);
	long fsize = ftell(stdin);
	fseek(stdin, 0, SEEK_SET);

	char * buffer = (char*)malloc(sizeof(char)*SUPERMAXPATH);
	//if there is stuff in the pipeline, get line per line
	if (fsize > 0) {
		//printf("getting from pipeline");
		while (fgets(buffer, SUPERMAXPATH, stdin) != NULL) {
			bool nlq = false;
			//again search for the newline char and replacing it with 0 so that there is no new line in the string
			for (int i = 0; i < SUPERMAXPATH && !nlq; i++) {
				if (buffer[i] == '\n' || buffer[i] == '\r') {
					buffer[i] = 0;
					nlq = true;
				}
			}
			//making a buffer for copying the file
			//using mbstowcs to convert to a widechar
			wchar_t * filealloc = (wchar_t*)malloc(sizeof(wchar_t)*SUPERMAXPATH); filealloc[0] = 0;
			size_t conv = { 0 };
			mbstowcs_s(&conv, filealloc, SUPERMAXPATH, buffer, strlen(buffer));


			//if file exists, add to the file stack
			if (PathFileExists(filealloc)) {
				files[filesc] = filealloc;
				filesc++;
			}
			else {
				wprintf(L"Path does not exist for file %ls\n", filealloc);
				free(filealloc);
			}
			buffer[0] = 0;
		}
	}
	free(buffer);


	//if more then 1 file, do a comparisson (check shared blocks)
	if (filesc > 1) {
		retvalue = comparefiles(bsf,files, filesc);
	} 
	//else dump the current file
	else if (filesc > 0) {
		retvalue = dumpfile(bsf,files[0]);
	}
	else {
		retvalue = 1;
		printf("Need at least 2 files to compare and 1 to dump");
	}
	
	//cleanup the output file, 
	CLEANUP:
	if (bsf->printerisfile) {
		fflush(bsf->printer);
		fclose(bsf->printer);
		free(bsf->printer);
	}

	//clean up buffers 
	//this program might have memory leaks but who cares for a program that is running 10 seconds ;)
	for (int i = 0; i < filesc; i++) {
		free(files[i]);
	}
	free(files);
	free(readfromfile);
    return retvalue;
}

