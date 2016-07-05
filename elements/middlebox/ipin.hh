#ifndef MIDDLEBOX_IPIN_HH
#define MIDDLEBOX_IPIN_HH

#include "ipelement.hh"
#include "stackelement.hh"
#include <click/element.hh>

CLICK_DECLS

class IPIn : public StackElement, public IPElement
{
public:
    IPIn() CLICK_COLD;

    const char *class_name() const        { return "IPIn"; }
    const char *port_count() const        { return PORTS_1_1; }
    const char *processing() const        { return PROCESSING_A_AH; }

    int configure(Vector<String> &, ErrorHandler *) CLICK_COLD;

protected:
    Packet* processPacket(struct fcb*, Packet*);
    void setPacketDirty(struct fcb*, WritablePacket*);
};

CLICK_ENDDECLS
#endif
