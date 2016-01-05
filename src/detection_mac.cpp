#include "detection.h"
#include "deviceList.h"

#include <CoreFoundation/CoreFoundation.h>

#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>

#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOCDMedia.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/storage/IOCDTypes.h>
#include <IOKit/storage/IOMediaBSDClient.h>

#include <IOKit/serial/IOSerialKeys.h>
#include <IOKit/serial/ioss.h>
#include <IOKit/IOMessage.h>

#include <DiskArbitration/DiskArbitration.h>
#include <DiskArbitration/DASession.h>
#include <DiskArbitration/DADisk.h>
#include <DiskArbitration/DADissenter.h>

#include <sys/param.h>
#include <pthread.h>
#include <unistd.h>
#include <uv.h>

#define dlog(fmt, arg...) printf("%s(%d) " fmt, __func__, __LINE__, ##arg)

// Get the current OSX version
const auto CURRENT_SUPPORTED_VERSION = __MAC_OS_X_VERSION_MAX_ALLOWED;
// El Capitan is 101100 (in AvailabilityInternal.h)
const auto EL_CAPITAN = 101100;

// IOUSBDevice has become IOUSBHostDevice in El Capitan
const char *SERVICE_MATCHER = CURRENT_SUPPORTED_VERSION < EL_CAPITAN ? "IOUSBDevice" : "IOUSBHostDevice";

typedef struct DeviceListItem
{
  io_object_t                 notification;
  IOUSBDeviceInterface**      deviceInterface;
  DeviceItem_t*               deviceItem;

} stDeviceListItem;

static IONotificationPortRef    gNotifyPort;
static io_iterator_t            gAddedIter;
static CFRunLoopRef             gRunLoop;

CFMutableDictionaryRef          matchingDict;
CFRunLoopSourceRef              runLoopSource;

static pthread_t                lookupThread;

pthread_mutex_t                 notify_mutex;
pthread_cond_t                  notifyNewDevice;
pthread_cond_t                  notifyDeviceHandled;

ListResultItem_t*               notify_item;

bool                            newDeviceAvailable = false;
bool                            deviceHandled      = true;
bool                            isAdded            = false;
bool                            isRunning          = false;
bool                            initialDeviceImport = true;


void WaitForDeviceHandled();
void SignalDeviceHandled();
void WaitForNewDevice();
void SignalDeviceAvailable();

char* cfStringRefToCString( CFStringRef cfString )
{
  if ( !cfString ) 
    return NULL;

  static char string[2048];

  string[0] = '\0';
  CFStringGetCString(cfString,
                     string,
                     MAXPATHLEN,
                     kCFStringEncodingASCII);

  return &string[0];
}

char* cfTypeToCString( CFTypeRef cfString )
{
  if ( !cfString ) return NULL;

  static char deviceFilePath[2048];

  deviceFilePath[0] = '\0';

  CFStringGetCString(CFCopyDescription(cfString),
                     deviceFilePath, MAXPATHLEN,
                     kCFStringEncodingASCII);

  char* p = deviceFilePath;

  while (*p != '\"')
    p++;

  p++;

  char* pp = p;

  while (*pp != '\"')
    pp++;

  *pp = '\0';

  if (isdigit(*p))
    *p = 'x';

  return p;
}

//================================================================================================
//
//  DeviceRemoved
//
//  This routine will get called whenever any kIOGeneralInterest notification happens.  We are
//  interested in the kIOMessageServiceIsTerminated message so that's what we look for.  Other
//  messages are defined in IOMessage.h.
//
//================================================================================================
void DeviceRemoved(void *refCon, io_service_t service, natural_t messageType, void *messageArgument)
{
  kern_return_t   kr;

  stDeviceListItem* deviceListItem = (stDeviceListItem *) refCon;

  DeviceItem_t* deviceItem = deviceListItem->deviceItem;

  if (messageType == kIOMessageServiceIsTerminated)
    {
      if (deviceListItem->deviceInterface)
        {
          kr = (*deviceListItem->deviceInterface)->Release(deviceListItem->deviceInterface);
        }

      kr = IOObjectRelease(deviceListItem->notification);


      ListResultItem_t* item = NULL;

      if (deviceItem)
        {
          item = CopyElement(&deviceItem->deviceParams);
          RemoveItemFromList(deviceItem);
          delete deviceItem;
        }

      else
        {
          item = new ListResultItem_t();
        }

      WaitForDeviceHandled();

      notify_item = item;
      isAdded     = false;

      SignalDeviceAvailable();

    }
}

//================================================================================================
//
//  DeviceAdded
//
//  This routine is the callback for our IOServiceAddMatchingNotification.  When we get called
//  we will look at all the devices that were added and we will:
//
//  1.  Create some private data to relate to each device (in this case we use the service's name
//      and the location ID of the device
//  2.  Submit an IOServiceAddInterestNotification of type kIOGeneralInterest for this device,
//      using the refCon field to store a pointer to our private data.  When we get called with
//      this interest notification, we can grab the refCon and access our private data.
//
//================================================================================================
void DeviceAdded(void *refCon, io_iterator_t iterator)
{
  kern_return_t           kr;
  io_service_t            usbDevice;
  IOCFPlugInInterface**   plugInInterface = NULL;
  SInt32                  score;
  HRESULT                 res;

  while ((usbDevice = IOIteratorNext(iterator)))
    {
      io_name_t       deviceName;
      CFStringRef     deviceNameAsCFString;
      UInt32          locationID;
      UInt16          vendorId;
      UInt16          productId;
      UInt16          addr;

      DeviceItem_t* deviceItem = new DeviceItem_t();

      // Get the USB device's name.
      kr = IORegistryEntryGetName(usbDevice, deviceName);

      if (KERN_SUCCESS != kr)
        {
          deviceName[0] = '\0';
        }

      deviceNameAsCFString = CFStringCreateWithCString(kCFAllocatorDefault, deviceName, kCFStringEncodingASCII);


      if (deviceNameAsCFString)
        {
          Boolean result;
          char    deviceName[MAXPATHLEN];

          // Convert from a CFString to a C (NUL-terminated)
          result = CFStringGetCString(deviceNameAsCFString,
                                      deviceName,
                                      sizeof(deviceName),
                                      kCFStringEncodingUTF8);

          if (result)
            {
              deviceItem->deviceParams.deviceName = deviceName;
            }

          CFRelease(deviceNameAsCFString);
        }

      CFStringRef manufacturerAsCFString = (CFStringRef) IORegistryEntrySearchCFProperty(usbDevice,
                                                                                         kIOServicePlane,
                                                                                         CFSTR(kUSBVendorString),
                                                                                         kCFAllocatorDefault,
                                                                                         kIORegistryIterateRecursively);

      if (manufacturerAsCFString)
        {
          Boolean result;
          char    manufacturer[MAXPATHLEN];

          // Convert from a CFString to a C (NUL-terminated)
          result = CFStringGetCString(manufacturerAsCFString,
                                      manufacturer,
                                      sizeof(manufacturer),
                                      kCFStringEncodingUTF8);

          if (result)
            {
              deviceItem->deviceParams.manufacturer = manufacturer;
            }

          CFRelease(manufacturerAsCFString);
        }

      CFStringRef serialNumberAsCFString = (CFStringRef) IORegistryEntrySearchCFProperty(usbDevice,
                                                                                         kIOServicePlane,
                                                                                         CFSTR(kUSBSerialNumberString),
                                                                                         kCFAllocatorDefault,
                                                                                         kIORegistryIterateRecursively);

      if (serialNumberAsCFString)
        {
          Boolean result;
          char    serialNumber[MAXPATHLEN];

          // Convert from a CFString to a C (NUL-terminated)
          result = CFStringGetCString(serialNumberAsCFString,
                                      serialNumber,
                                      sizeof(serialNumber),
                                      kCFStringEncodingUTF8);

          if (result)
            {
              deviceItem->deviceParams.serialNumber = serialNumber;
            }

          CFRelease(serialNumberAsCFString);
        }

      CFStringRef bsdName = NULL;

      // Block for a while and keep trying to see if the device has been mounted,
      // as this procedure can take some time.
      for(int i = 0; !initialDeviceImport && i < 50; ++i)
        {
          bsdName = (CFStringRef) IORegistryEntrySearchCFProperty(usbDevice,
                                                                  kIOServicePlane,
                                                                  CFSTR(kIOBSDNameKey),
                                                                  kCFAllocatorDefault,
                                                                  kIORegistryIterateRecursively);

          if (bsdName)
            {
              char bsdNameBuf[4096];
              sprintf( bsdNameBuf, "/dev/%ss1", cfStringRefToCString(bsdName));
              char* bsdNameC = &bsdNameBuf[0];
              DASessionRef daSession = DASessionCreate(kCFAllocatorDefault);

              DADiskRef disk = DADiskCreateFromBSDName(kCFAllocatorDefault, daSession, bsdNameC);

              if (disk)
                {
                  // The device is mounted, but we have to wait for the disk volume to mount.
                  for(int j = 0; j < 50; ++j)
                    {
                      CFDictionaryRef desc = DADiskCopyDescription(disk);

                      if (desc)
                        {
                          //CFTypeRef str = CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey);
                          CFTypeRef str = CFDictionaryGetValue(desc, kDADiskDescriptionVolumeNameKey);
                          char* volumeName = cfTypeToCString(str);

                          if (volumeName && strlen(volumeName))
                            {
                              char volumePath[MAXPATHLEN];

                              sprintf(volumePath, "/Volumes/%s", volumeName);

                              deviceItem->deviceParams.mountPath = volumePath;

                              CFRelease(desc);
                              break;
                            }
                          else
                            {
                              CFRelease(desc);
                            }
                        }
                      else
                        {
                          // We didn't get a volume yet, so just 0.1 seconds.
                          // Total timeout should be 5 seconds.
                          usleep(100000);
                        }
                    }

                  CFRelease(disk);
                }

              CFRelease(daSession);
              CFRelease(bsdName);
              break;
            }
          else
            {
              // We didn't get a BSD name, so just wait 0.15 seconds.
              // In total, timeout should be 7.5 seconds
              usleep(150000);
            }
        }



      // Now, get the locationID of this device. In order to do this, we need to create an IOUSBDeviceInterface
      // for our device. This will create the necessary connections between our userland application and the
      // kernel object for the USB Device.
      kr = IOCreatePlugInInterfaceForService(usbDevice,
                                             kIOUSBDeviceUserClientTypeID,
                                             kIOCFPlugInInterfaceID,
                                             &plugInInterface,
                                             &score);

      if ((kIOReturnSuccess != kr) || !plugInInterface)
        {
          fprintf(stderr, "IOCreatePlugInInterfaceForService returned 0x%08x.\n", kr);
          continue;
        }

      stDeviceListItem *deviceListItem = new stDeviceListItem();

      // Use the plugin interface to retrieve the device interface.
      res = (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID*) &deviceListItem->deviceInterface);

      // Now done with the plugin interface.
      (*plugInInterface)->Release(plugInInterface);

      if (res || deviceListItem->deviceInterface == NULL)
        {
          fprintf(stderr, "QueryInterface returned %d.\n", (int) res);
          continue;
        }

      // Now that we have the IOUSBDeviceInterface, we can call the routines in IOUSBLib.h.
      // In this case, fetch the locationID. The locationID uniquely identifies the device
      // and will remain the same, even across reboots, so long as the bus topology doesn't change.

      kr = (*deviceListItem->deviceInterface)->GetLocationID(deviceListItem->deviceInterface, &locationID);

      if (KERN_SUCCESS != kr)
        {
          fprintf(stderr, "GetLocationID returned 0x%08x.\n", kr);
          continue;
        }

      deviceItem->deviceParams.locationId = locationID;


      kr = (*deviceListItem->deviceInterface)->GetDeviceAddress(deviceListItem->deviceInterface, &addr);

      if (KERN_SUCCESS != kr)
        {
          fprintf(stderr, "GetDeviceAddress returned 0x%08x.\n", kr);
          continue;
        }

      deviceItem->deviceParams.deviceAddress = addr;


      kr = (*deviceListItem->deviceInterface)->GetDeviceVendor(deviceListItem->deviceInterface, &vendorId);

      if (KERN_SUCCESS != kr)
        {
          fprintf(stderr, "GetDeviceVendor returned 0x%08x.\n", kr);
          continue;
        }

      deviceItem->deviceParams.vendorId = vendorId;

      kr = (*deviceListItem->deviceInterface)->GetDeviceProduct(deviceListItem->deviceInterface, &productId);

      if (KERN_SUCCESS != kr)
        {
          fprintf(stderr, "GetDeviceProduct returned 0x%08x.\n", kr);
          continue;
        }

      deviceItem->deviceParams.productId = productId;


      // Extract path name as unique key
      io_string_t pathName;

      IORegistryEntryGetPath(usbDevice, kIOServicePlane, pathName);

      deviceNameAsCFString = CFStringCreateWithCString(kCFAllocatorDefault, pathName, kCFStringEncodingASCII);

      char cPathName[MAXPATHLEN];

      if (deviceNameAsCFString)
        {
          Boolean result;

          // Convert from a CFString to a C (NUL-terminated)
          result = CFStringGetCString(deviceNameAsCFString,
                                      cPathName,
                                      sizeof(cPathName),
                                      kCFStringEncodingUTF8);


          CFRelease(deviceNameAsCFString);
        }

      AddItemToList(cPathName, deviceItem);

      deviceListItem->deviceItem = deviceItem;

      if (initialDeviceImport == false)
        {
          WaitForDeviceHandled();
          notify_item = &deviceItem->deviceParams;
          isAdded = true;
          SignalDeviceAvailable();
        }

      // Register for an interest notification of this device being removed. Use a reference to our
      // private data as the refCon which will be passed to the notification callback.
      kr = IOServiceAddInterestNotification(gNotifyPort,                      // notifyPort
                                            usbDevice,                        // service
                                            kIOGeneralInterest,               // interestType
                                            DeviceRemoved,                    // callback
                                            deviceListItem,                   // refCon
                                            & (deviceListItem->notification)  // notification
                                            );

      if (KERN_SUCCESS != kr)
        {
          printf("IOServiceAddInterestNotification returned 0x%08x.\n", kr);
        }

      // Done with this USB device; release the reference added by IOIteratorNext
      kr = IOObjectRelease(usbDevice);
    }
}


void WaitForDeviceHandled()
{
  pthread_mutex_lock(&notify_mutex);

  if (deviceHandled == false)
    {
      pthread_cond_wait(&notifyDeviceHandled, &notify_mutex);
    }

  deviceHandled = false;
  pthread_mutex_unlock(&notify_mutex);
}

void SignalDeviceHandled()
{
  pthread_mutex_lock(&notify_mutex);
  deviceHandled = true;
  pthread_cond_signal(&notifyDeviceHandled);
  pthread_mutex_unlock(&notify_mutex);
}

void WaitForNewDevice()
{
  pthread_mutex_lock(&notify_mutex);

  if (newDeviceAvailable == false)
    {
      pthread_cond_wait(&notifyNewDevice, &notify_mutex);
    }

  newDeviceAvailable = false;
  pthread_mutex_unlock(&notify_mutex);
}

void SignalDeviceAvailable()
{
  pthread_mutex_lock(&notify_mutex);
  newDeviceAvailable = true;
  pthread_cond_signal(&notifyNewDevice);
  pthread_mutex_unlock(&notify_mutex);
}


void *RunLoop(void * arg)
{

  runLoopSource = IONotificationPortGetRunLoopSource(gNotifyPort);

  gRunLoop = CFRunLoopGetCurrent();
  CFRunLoopAddSource(gRunLoop, runLoopSource, kCFRunLoopDefaultMode);

  // Start the run loop. Now we'll receive notifications.
  CFRunLoopRun();

  // We should never get here
  fprintf(stderr, "Unexpectedly back from CFRunLoopRun()!\n");

  return NULL;
}

void NotifyAsync(uv_work_t* req)
{
  WaitForNewDevice();
}

void NotifyFinished(uv_work_t* req)
{
  if (isRunning)
    {
      if (isAdded)
        {
          NotifyAdded(notify_item);
        }

      else
        {
          NotifyRemoved(notify_item);
        }
    }

  // Delete Item in case of removal
  if (isAdded == false)
    {
      delete notify_item;
    }

  if (isRunning)
    {
      uv_queue_work(uv_default_loop(), req, NotifyAsync, (uv_after_work_cb)NotifyFinished);
    }

  SignalDeviceHandled();
}

void Start()
{
  isRunning = true;
}

void Stop()
{
  isRunning = false;
  pthread_mutex_lock(&notify_mutex);
  pthread_cond_signal(&notifyNewDevice);
  pthread_mutex_unlock(&notify_mutex);
}

void InitDetection()
{

  kern_return_t           kr;

  // Set up the matching criteria for the devices we're interested in. The matching criteria needs to follow
  // the same rules as kernel drivers: mainly it needs to follow the USB Common Class Specification, pp. 6-7.
  // See also Technical Q&A QA1076 "Tips on USB driver matching on Mac OS X"
  // <http://developer.apple.com/qa/qa2001/qa1076.html>.
  // One exception is that you can use the matching dictionary "as is", i.e. without adding any matching
  // criteria to it and it will match every IOUSBDevice in the system. IOServiceAddMatchingNotification will
  // consume this dictionary reference, so there is no need to release it later on.

  matchingDict = IOServiceMatching(SERVICE_MATCHER);    // Interested in instances of class
  // IOUSBDevice and its subclasses

  if (matchingDict == NULL)
    {
      fprintf(stderr, "IOServiceMatching returned NULL.\n");
    }

  // Create a notification port and add its run loop event source to our run loop
  // This is how async notifications get set up.

  gNotifyPort = IONotificationPortCreate(kIOMasterPortDefault);

  // Now set up a notification to be called when a device is first matched by I/O Kit.
  kr = IOServiceAddMatchingNotification(gNotifyPort,                  // notifyPort
                                        kIOFirstMatchNotification,    // notificationType
                                        matchingDict,                 // matching
                                        DeviceAdded,                  // callback
                                        NULL,                         // refCon
                                        &gAddedIter                   // notification
                                        );

  if (KERN_SUCCESS != kr)
    {
      printf("IOServiceAddMatchingNotification returned 0x%08x.\n", kr);
    }

  // Iterate once to get already-present devices and arm the notification
  DeviceAdded(NULL, gAddedIter);

  initialDeviceImport = false;

  pthread_mutex_init(&notify_mutex, NULL);
  pthread_cond_init(&notifyNewDevice, NULL);
  pthread_cond_init(&notifyDeviceHandled, NULL);

  int rc = pthread_create(&lookupThread, NULL, RunLoop, NULL);

  if (rc)
    {
      printf("ERROR; return code from pthread_create() is %d\n", rc);
      exit(-1);
    }

  uv_work_t* req = new uv_work_t();

  uv_queue_work(uv_default_loop(), req, NotifyAsync, (uv_after_work_cb)NotifyFinished);

  Start();
}

void EIO_Find(uv_work_t* req)
{
  ListBaton* data = static_cast<ListBaton*>(req->data);

  CreateFilteredList(&data->results, data->vid, data->pid);
}
