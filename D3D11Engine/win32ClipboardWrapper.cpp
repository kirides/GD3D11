#include "pch.h"
#include "win32ClipboardWrapper.h"
#include <stdio.h>

int clipput(char *toclipdata)
{
	char far *buffer;
	unsigned int bytes;

	HGLOBAL clipbuffer;

	// Set data buffer length to accomodate toclipdata
	bytes = strlen(toclipdata);

	// Peform the transfer to clipboard magic
	OpenClipboard(nullptr);
	EmptyClipboard();
	clipbuffer = GlobalAlloc(GMEM_DDESHARE,bytes+1);
    if ( clipbuffer == nullptr ) {
        return GetLastError() * -1; // Do what you want to signal error
    }
	buffer = (char far*)GlobalLock(clipbuffer);
    if ( buffer == nullptr ) {
		return GetLastError() * -1; // Do what you want to signal error
    }
	strcpy(buffer,toclipdata);

	GlobalUnlock(clipbuffer);
	SetClipboardData(CF_TEXT,clipbuffer);
	CloseClipboard();

	// Return byte count
	return bytes; // non-negative value is success.
}

char *clipget(int &bytes)
	{
		int k;
		char *data = nullptr;
		char empty[80] = "<Clipboard is empty>";

		bytes = 0;

		// open the clipboard
		if ( OpenClipboard( nullptr ) )
		{
			HANDLE hData = GetClipboardData( CF_TEXT );
			char* clipboardBuffer = hData ? static_cast<char*>(GlobalLock( hData )) : nullptr;

			if ( clipboardBuffer == nullptr )
			{
				bytes = strlen( empty );
				data = static_cast<char*>(malloc( bytes + 1 ));
				if ( data ) {
					strcpy( data, empty );
				}
			}
			else
			{
				bytes = strlen( clipboardBuffer );
				data = static_cast<char*>(malloc( bytes + 1 ));
				if ( data ) {
					strcpy( data, clipboardBuffer );
				}

				GlobalUnlock( hData );
			}

			CloseClipboard();
		}
		// Return an open clipboard failed message
		else
		{
			k = GetLastError();
			if ( k < 0 )
				bytes = k;
			else
				bytes = k * -1;
			sprintf( empty, "Error occurred opening clipboard - RC: %d", k );
			k = strlen( empty );
			data = static_cast<char*>(malloc( k + 1 ));
			if ( data ) {
				strcpy( data, empty );
			}
		}
		// Return pointer to data field allocated
		// It's up to the caller to free the storage
		return data;
	}
