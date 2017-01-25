#include <click/modificationlist.hh>
#include <click/config.h>
#include <click/glue.hh>
#ifdef CLICK_USERLEVEL
#include <math.h>
#endif
#include <click/memorypool.hh>

/*
 * modificationlist.cc - Class used to store the modifications performed in a
 * packet's structure
 *
 * Romain Gaillard.
 */

ModificationList::ModificationList(MemoryPool<struct ModificationNode> *poolNodes)
{
    this->poolNodes = poolNodes;
    head = NULL;
    committed = false;
}

ModificationList::~ModificationList()
{
    clear();
}

void ModificationList::clear()
{
    bool freed = false;

    struct ModificationNode* node = head;

    // Browse the list to free remaining nodes
    while(node != NULL)
    {
        freed = true; // Indicate that we had to free a node
        struct ModificationNode* next = node->next;
        poolNodes->releaseMemory(node);
        node = next;
    }

    if(freed)
    {
        click_chatter("Warning: modifications in a packet were not committed "
            "before destroying the list");
    }
}

void ModificationList::printList()
{
    struct ModificationNode* node = head;

    // Browse the linked list and display the information of each node
    click_chatter("--- Modification list ---");
    while(node != NULL)
    {
        click_chatter("(%u: %d)", node->position, node->offset);

        node = node->next;
    }
}

bool ModificationList::addModification(unsigned int position, int offset)
{
    // The structure refuses new modifications if a commit has been made before
    if(committed)
        return false;

    struct ModificationNode* prev = NULL;
    struct ModificationNode* node = head;

    // Determine where to add the modification in the list
    while(node != NULL && node->position <= position)
    {
        // We translate the requested position which is relative to the current
        // content of the packet to a position relative to the initial content
        // of the packet
        if(node->position < position)
        {
            /* Apply the offset of the current node on the requested position
             * A positive offset means that we added data in the new content
             * A negative offset means that we removed data in the new content
             * Thus, we substract the offset
             */
            unsigned int newPosition = 0;

            if(node->offset > 0 && node->offset > position)
                newPosition = 0;
            else
                newPosition = position - node->offset;

            // Ensure that we do not go beyond the position of the current node
            if(newPosition < node->position)
                newPosition = node->position;

            // Update the position
            position = newPosition;
        }

        prev = node;
        node = node->next;
    }

    // We went one node too far during the exploration so get back the previous
    // node
    node = prev;

    // Add the node in the linked list
    // Case 1: Reuse an existing node (same positions)
    if(node != NULL && node->position == position)
    {
        // In this case, we simply add the effect of the new modification
        // to the existing node
        node->offset += offset;
    }
    else
    {
        // Case 2: Create a new one
        struct ModificationNode* newNode = poolNodes->getMemory();
        newNode->position = position;
        newNode->offset = offset;

        if(node == NULL)
        {
            // Case 2a: Add the node at the beginning of the list
            newNode->next = head;
            head = newNode;
        }
        else
        {
            // Case 2b: Add the node in the middle or the end of the list
            newNode->next = node->next;
            node->next = newNode;
        }
    }

    // Try to merge node to reduce their number
    /* Example where merge is required:
     * We have "abcdefgh"
     * We remove "ef" and add to the list: (4, -2)
     * We thus have "abcdgh"
     * We now remove "bcdg", and thus we add to the list (1, -4)
     * The list is thus ((1, -4), (4, -2))
     * We need to merge those two entries into (1, -6)
     */
    mergeNodes();

    return true;
}

void ModificationList::mergeNodes()
{
    struct ModificationNode *prev = NULL;
    struct ModificationNode *node = head;

    // Browse the list of nodes
    while(node != NULL)
    {
        bool merged = false;

        if(prev != NULL)
        {
            // If the previous node exists, we will determine if the data
            // of the current node can be merged with the previous one

            // Determine the range of the modification of the previous node
            unsigned int range = prev->position + (int)abs(prev->offset);

            // If the modification of this node is within the range of the
            // previous one and if they represent the same kind of modification
            // (both a deletion or an insertion), merge them
            if(node->position < range && sameSign(prev->offset, node->offset))
            {
                // Remove current node and merge its value with the previous node
                prev->offset += node->offset;
                prev->next = node->next;
                poolNodes->releaseMemory(node);

                merged = true;
            }
        }

        // Determine the next element
        if(merged)
            node = prev->next;
        else
        {
            prev = node;
            node = node->next;
        }
    }
}

void ModificationList::commit(ByteStreamMaintainer &maintainer)
{
    struct ModificationNode* node = head;

    // Get the last value in the tree to obtain the effects of
    // the modifications in the previous packets
    // Ack offsets have the opposite sign as the elements in the modification list
    int offsetTotal = -(maintainer.lastOffsetInAckTree());

    while(node != NULL)
    {
        // Accumulate on the position the effects of the previous modifications
        unsigned int newPositionAck = node->position + offsetTotal;
        offsetTotal += node->offset;

        if(node->offset > 0 && offsetTotal < 0)
        {
            /* When we insert bytes in the packets, by default, when committed,
             * it will create an entry in the tree at the position of the
             * insertion with a negative offset (corresponding to the number of
             * bytes inserted).
             * When we convert a position, it will decrease this position
             * by the offset PROVIDED THAT the new position is not below the
             * position of the node.
             * e.g: we insert 4 bytes at the positon 3, if we commit, it creates
             * an entry in the tree (3, -4). Thus, all the positions in the
             * interval [3, 7] will be converted to 3 (to ensure that the
             * value, to which the offset has been substracted, is at least equal
             * to the position of the node).
             *
             * A problem occurs when modifications occuring earlier in the stream
             * result in a positive total offset. For instance, if we had the key
             * (1, 6), the modification described above will create the key
             * (3, 2) and we can see that the offset is now positive.
             * This is problematic as we do not have the behaviour described
             * above anymore: no interval will be converted in a given key.
             *
             * To overcome this problem, we insert two keys instead of one.
             * The first one at the beginning of the insertion that will
             * have an offset to which the length of the insertion is added
             * The second key is added at the position after the insertion
             * with the original offset.
             * With the example above, we now have three keys:
             * (1, 6)
             * (3, 6) (6 is 2 (original offset) + 4 (length of the insertion))
             * (7, 2) (7 is 3 (original position) + 4 (length of the insertion))
             *
             * Thus, if we request a position in the inserted area, we will
             * obtain a position after this area, which will be corrected to be
             * at most the position of the insertion (here 7 + 2 = 9)
             * during the mapping. Therefore, every position in the interval
             * [3, 7] will be converted to the same position, 9, and we get back
             * the behaviour we wanted
             */

            // We add the length of the insertion to the new offset
            int insertedOffset = -(offsetTotal - node->offset);

            maintainer.insertInAckTree(newPositionAck, insertedOffset);

            newPositionAck += (unsigned)node->offset;
        }

        // The position of the SEQ mapping remains unchanged
        unsigned int newPositionSeq = node->position;

        // Accumulate on the offsets the effects of the previous modifications
        int newOffsetAck = offsetTotal;
        int newOffsetSeq = offsetTotal;

        // The modification to apply to perform the mapping for the ACK has the
        // opposite sign as the modification that was performed so that they
        // counterbalance
        newOffsetAck = -(newOffsetAck);

        // Insert the node in the tree. In case of duplicate, keep only the
        // new value
        maintainer.insertInAckTree(newPositionAck, newOffsetAck);
        maintainer.insertInSeqTree(newPositionSeq, newOffsetSeq);

        struct ModificationNode* next = node->next;

        // Remove the node from the list and release memory
        poolNodes->releaseMemory(node);
        node = next;

        // We process the list element by element so the new head is the next node
        head = node;
    }
}

inline bool ModificationList::sameSign(int x, int y)
{
    return ((x <= 0) == (y <= 0));
}