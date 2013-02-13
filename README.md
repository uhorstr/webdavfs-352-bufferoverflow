## webdavfs-352-bufferoverflow

Using the WebDAV client on MAC OS X (webdavfs/3.0.0, Darwin 13.0.0) with a 
WebDAV server resulted in a random connection loss indicated by a removal of
the associated entry in the Finder "Shared" section.

Based on code review, this connection loss was triggered by a segmentation fault
on the client side due to a memory corruption in the WebDAV implementation.

Depending on the size of the lock token issued by the server, the crash pattern
differed. The root cause was an incorrect string allocation size in the client
implementation.

The patch was applied with webdavfs-367.
