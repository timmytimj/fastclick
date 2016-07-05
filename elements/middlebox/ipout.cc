#include <click/config.h>
#include <click/router.hh>
#include <click/args.hh>
#include <click/error.hh>
#include <clicknet/ip.h>
#include "ipout.hh"

CLICK_DECLS

IPOut::IPOut()
{

}

int IPOut::configure(Vector<String> &conf, ErrorHandler *errh)
{
    return 0;
}

Packet* IPOut::processPacket(struct fcb*, Packet* p)
{
    WritablePacket *packet = p->uniqueify();

    // Recompute the IP checksum if the packet has been modified
    if(getAnnotationDirty(packet))
        computeIPChecksum(packet);

    return p;
}

CLICK_ENDDECLS
EXPORT_ELEMENT(IPOut)
ELEMENT_REQUIRES(IPElement)
