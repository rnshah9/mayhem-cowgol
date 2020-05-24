#include "globals.h"
#include "midcodes.h"

#define IMPL
#include "codegen.h"

#define NUM_INSTRUCTIONS 200
static Instruction instructions[NUM_INSTRUCTIONS];
static int instructioncount;
static int maxinstructioncount = 0;

#define NUM_NODES 200
static Node* nodes[NUM_NODES];
static int nodecount;
static int maxnodecount = 0;

void unmatched_instruction(Node* node)
{
	fprintf(stderr, "No rule matches 0x%x := ", node->desired_reg);
	print_midnode(stderr, node);
	fprintf(stderr, "\n");
	fatal("Internal compiler error");
}

bool template_comparator(const uint8_t* data, const uint8_t* template)
{
	int i = INSTRUCTION_TEMPLATE_DEPTH;
	while (i--)
	{
		uint8_t d = *data++;
		uint8_t t = *template++;
		if (t && (d != t))
			return false;
	}
	return true;
}

void push_node(Node* node)
{
	if (nodecount == NUM_NODES)
		fatal("ran out of nodes");
	nodes[nodecount++] = node;
	if (nodecount > maxnodecount)
		maxnodecount = nodecount;
}

static reg_t findfirst(reg_t reg)
{
	for (int i=0; i<REGISTER_COUNT; i++)
	{
		if (reg & (1<<i))
			return 1<<i;
	}
	return 0;
}

static void deadlock(int ruleid)
{
	while (instructioncount != 0)
	{
		Instruction* insn = &instructions[--instructioncount];
		#if 1
		printf("insn %d ruleid %d produces 0x%x inputs 0x%x outputs 0x%x\n",
			insn - instructions,
			insn->ruleid,
			insn->produced_reg,
			insn->input_regs,
			insn->output_regs);
		for (int i=1; i<INSTRUCTION_TEMPLATE_DEPTH; i++)
		{
			Node* n = insn->n[i];
			if (n && n->produced_reg)
				printf("  consumes 0x%x from insn %d\n",
					n->produced_reg, n->producer - instructions);
		}
		#endif
	}

	fatal("register allocation deadlock (rule %d contains impossible situation)", ruleid);
}

static reg_t find_conflicting_registers(reg_t reg)
{
	reg_t conflicting = 0;
	for (int i=0; i<REGISTER_COUNT; i++)
	{
		const Register* r = &registers[i];
		if (r->id & reg)
			conflicting |= r->uses;
	}
	return conflicting;
}

static bool isstacked(reg_t reg)
{
	for (int i=0; i<REGISTER_COUNT; i++)
	{
		const Register* r = &registers[i];
		if (r->id & reg)
			return r->isstacked;
	}
	assert(false);
}

static Regmove* create_spill(Instruction* instruction, reg_t src, reg_t dest)
{
	Regmove* spill = calloc(sizeof(Regmove), 1);
	spill->src = src;
	spill->dest = dest;
	spill->next = instruction->first_spill;
	instruction->first_spill = spill;
	return spill;
}

static Regmove* create_reload(Instruction* instruction, reg_t src, reg_t dest)
{
	Regmove* reload = calloc(sizeof(Regmove), 1);
	reload->src = src;
	reload->dest = dest;
	if (!instruction->first_reload)
		instruction->first_reload = reload;
	if (instruction->last_reload)
		instruction->last_reload->next = reload;
	instruction->last_reload = reload;
	return reload;
}

static reg_t calculate_blocked_registers(Instruction* start, Instruction* end)
{
	reg_t blocked = 0;
	while (start <= end)
	{
		blocked |= (start->input_regs | start->output_regs);
		start++;
	}
	return blocked;
}

static void block_registers(Instruction* start, Instruction* end, reg_t blocked)
{
	while (start <= end)
	{
		start->input_regs |= blocked;
		start->output_regs |= blocked;
		start++;
	}
}

/* Deal with the theoretically simple but practically really, really annoying
 * problem of moving one set of registers to another. All register-to-
 * register moves have to happen simultaneously, so it's perfectly legal
 * for the move set to consist of A->B, B->A... which means the two registers
 * have to be swapped.
 */
static void shuffle_registers(Regmove* moves)
{
	reg_t dests = 0;
	reg_t srcs = 0;

	Regmove* m = moves;
	while (m)
	{
		#if 0
		arch_emit_comment("spill/reload 0x%x -> 0x%x", m->src, m->dest);
		#endif
		dests |= m->dest;
		srcs |= m->src;
		m = m->next;
	}

	for (;;)
	{
		/* Attempt to do any pushes *first*, which frees up sources. */

		m = moves;
		while (m)
		{
			if (m->src && !m->dest)
				break;
			m = m->next;
		}
		if (m)
		{
			arch_emit_move(m->src, 0);
			srcs &= ~m->src;
			m->src = 0;
			continue;
		}

		/* Attempt to find a move into a register which is *not* a source
		 * (and is therefore completely safe). */

		m = moves;
		while (m)
		{
			if (m->src && m->dest && !(m->dest & srcs))
				break;
			m = m->next;
		}
		if (m)
		{
			arch_emit_move(m->src, m->dest);
			srcs &= ~m->src;
			dests &= ~m->dest;
			m->src = m->dest = 0;
			continue;
		}

		/* Only once we're done with pushes and register-to-register moves
		 * do we deal with pops. */

		m = moves;
		while (m)
		{
			if (!m->src && m->dest)
				break;
			m = m->next;
		}
		if (m)
		{
			arch_emit_move(0, m->dest);
			dests &= ~m->dest;
			m->dest = 0;
			continue;
		}

		/* If we got here and there are any undealt with moves, there's a move
		 * loop which we need to break somehow. The best thing is to stash a
		 * value into a temporary register but that gets gnarly if there aren't
		 * any left. So, we do it the brute-force way and stack something. */

		m = moves;
		while (m)
		{
			if (m->src || m->dest)
				break;
			m = m->next;
		}
		if (m)
		{
			reg_t stacked = m->src;
			arch_emit_move(stacked, 0);
			srcs &= ~m->src;
			m->src = 0; /* convert this to a pop */
			continue;
		}

		/* Nothing left to do. */
		break;
	}
}

void generate(Node* node)
{
	#if 0
	print_midnode(stdout, node);
	printf("\n");
	#endif

	memset(instructions, 0, sizeof(instructions));
	memset(nodes, 0, sizeof(nodes));
	instructioncount = 0;
	nodecount = 0;

	push_node(node);

	while (nodecount != 0)
	{
		if (instructioncount == NUM_INSTRUCTIONS)
			fatal("instruction tree too big");
		Instruction* producer = &instructions[instructioncount++];
		if (instructioncount > maxinstructioncount)
			maxinstructioncount = instructioncount;

		/* Find the first matching rule for this instruction. */

		Node* n = nodes[--nodecount];
	rewrite:;
		uint8_t matchbytes[INSTRUCTION_TEMPLATE_DEPTH] = {0};
		Node* nodes[INSTRUCTION_TEMPLATE_DEPTH] = {n};
		populate_match_buffer(producer, nodes, matchbytes);

		const Rule* r;
		int ruleid;
		for (ruleid=0; ruleid<INSTRUCTION_TEMPLATE_COUNT; ruleid++)
		{
			r = &codegen_rules[ruleid];
			if (!(r->flags & RULE_HAS_REWRITER))
			{
				/* If this is a generation rule, not a rewrite rule, check to
				 * make sure the rule actually applies to this node. */

				if (r->compatible_producable_regs)
				{
					/* This rule produces a result, so only match if the register
					 * is compatible. */
					if (!(n->desired_reg & r->compatible_producable_regs))
						continue;
				}
				else
				{
					/* This rules produces no result, so only match if we likewise
					 * don't want one. */
					if (n->desired_reg)
						continue;
				}
			}

			/* Check that the tree matches. */

			if (!template_comparator(matchbytes, r->matchbytes))
				continue;

			/* If there's a manual predicate, check that too. */

			if ((r->flags & RULE_HAS_PREDICATES) && !match_predicate(ruleid, nodes))
				continue;

			/* This rule matches! */

			goto matched;
		}
		unmatched_instruction(n);
	matched:

		/* If this is a rewrite rule, apply it now and return to the matcher. */

		if (r->flags & RULE_HAS_REWRITER)
		{
			Node* nr = rewrite_node(ruleid, nodes);
			nr->desired_reg = n->desired_reg;
			nr->consumer = n->consumer;

			/* Remember to patch any pointers to the old nodes in the node's
			 * consumer. */
			for (int i=0; i<INSTRUCTION_TEMPLATE_DEPTH; i++)
				if (n->consumer->n[i] == n)
					n->consumer->n[i] = nr;
			n = nr;
			goto rewrite;
		}

		/* We have a matching instruction, so set it up. */

		producer->ruleid = ruleid;
		producer->producable_regs = r->producable_regs;
		producer->output_regs = r->uses_regs;

		uint8_t copymask = r->copyable_nodes;
		uint8_t regmask = r->register_nodes;
		for (int i=0; i<INSTRUCTION_TEMPLATE_DEPTH; i++)
		{
			Node* n = nodes[i];
			if (copymask & 1)
			{
				producer->n[i] = n;
				if (regmask & 1)
				{
					push_node(n);
					n->desired_reg = r->consumable_regs[i];
					n->consumer = producer;
				}
			}
			copymask >>= 1;
			regmask >>= 1;
		}

		n->producer = producer;

		if (producer->producable_regs)
		{
			/* The instruction has produced a register. For stackable registers,
			 * stop now: we ignore them for doing actual register allocation. */

			if (!isstacked(producer->producable_regs))
			{
				for (;;)
				{
					/* Locate the register's consumer and allocate something. */

					Instruction* consumer = n->consumer;
					assert(consumer);
					reg_t blocked = calculate_blocked_registers(consumer+1, producer-1);

					reg_t current = n->desired_reg
							& producer->producable_regs 
							& ~(blocked | producer->output_regs | consumer->input_regs);
					if (current)
					{
						/* Good news --- we can allocate the ideal register for both
						 * producer and consumer. */

						current = findfirst(current);
						n->produced_reg = producer->produced_reg = current;

						blocked = find_conflicting_registers(current);
						consumer->input_regs |= blocked;
						block_registers(consumer+1, producer-1, blocked);
						producer->output_regs |= blocked;
						break;
					}

					current = producer->producable_regs & ~(blocked | producer->output_regs);
					if (current)
					{
						/* The producer and consumer want different registers, but the
						 * producer's register works up until the consumer. */

						reg_t producerreg = findfirst(current);
						reg_t consumerreg = findfirst(n->desired_reg & ~consumer->input_regs);
						if (consumerreg)
						{
							producer->produced_reg = producerreg;
							n->produced_reg = consumerreg;

							consumer->input_regs |= find_conflicting_registers(n->produced_reg);
							blocked = find_conflicting_registers(producer->produced_reg);
							block_registers(consumer+1, producer-1, blocked);
							producer->output_regs |= blocked;
							create_reload(consumer, producer->produced_reg, n->produced_reg);
							break;
						}
					}

					current = n->desired_reg & ~(blocked | consumer->input_regs);
					if (current)
					{
						/* The producer and consumer want different registers, but the
						 * consumer's register works after the producer. */

						reg_t producerreg = findfirst(
							producer->producable_regs & ~producer->output_regs);
						reg_t consumerreg = findfirst(current);
						if (producerreg)
						{
							producer->produced_reg = producerreg;
							n->produced_reg = consumerreg;

							blocked = find_conflicting_registers(n->produced_reg);
							consumer->input_regs |= blocked;
							block_registers(consumer+1, producer-1, blocked);
							producer->output_regs |= find_conflicting_registers(producer->produced_reg);
							create_spill(producer, producer->produced_reg, n->produced_reg);
							break;
						}
					}

					/* Bad news --- we can't allocate any registers. So, spill to the stack. */

					current = producer->producable_regs & ~producer->output_regs;
					if (!current)
						deadlock(ruleid);
					producer->produced_reg = findfirst(current);
					producer->output_regs |= find_conflicting_registers(producer->produced_reg);
					create_spill(producer, producer->produced_reg, 0);

					current = n->desired_reg & ~consumer->input_regs;
					if (!current)
						deadlock(ruleid);
					n->produced_reg = findfirst(current);
					consumer->input_regs |= find_conflicting_registers(n->produced_reg);
					create_reload(consumer, 0, n->produced_reg);
					break;
				}
			}

			/* If any nodes which produce values consumed by this instruction
			 * have registers which depend on the one produced by this
			 * instruction, update them now. */

			bool updated = false;
			for (int i=0; i<INSTRUCTION_TEMPLATE_DEPTH; i++)
			{
				Node* n = producer->n[i];
				if (n && (n->desired_reg == REG_SAME_AS_INSTRUCTION_RESULT))
				{
					n->desired_reg = producer->produced_reg;
					updated = true;
				}
			}

			/* If we *did* update a register, then the *other* register
             * requirements must be updated to blacklist that register,
			 * or we very quickly run into register deadlock. */

			if (updated)
			{
				for (int i=0; i<INSTRUCTION_TEMPLATE_DEPTH; i++)
				{
					Node* n = producer->n[i];
					if (n && (n->desired_reg != producer->produced_reg))
						n->desired_reg &= ~producer->produced_reg;
				}
			}
		}
	}

	/* Work backwards through the set of generated instructions, emitting each
	 * one. */

	while (instructioncount != 0)
	{
		Instruction* insn = &instructions[--instructioncount];
		#if 0
		printf("insn %d produces 0x%x inputs 0x%x outputs 0x%x\n",
			insn - instructions,
			insn->produced_reg,
			insn->input_regs,
			insn->output_regs);
		for (int i=1; i<INSTRUCTION_TEMPLATE_DEPTH; i++)
		{
			Node* n = insn->n[i];
			if (n && n->produced_reg)
				printf("consumes 0x%x from insn %d\n",
					n->produced_reg, n->producer - instructions);
		}
		#endif
		
		/* Emit reloads. */

		shuffle_registers(insn->first_reload);
		while (insn->first_reload)
		{
			Regmove* r = insn->first_reload;
			insn->first_reload = r->next;
			free(r);
		}

		/* The instruction itself! */

		emit_one_instruction(insn->ruleid, insn);

		/* Emit spills. */

		shuffle_registers(insn->first_spill);
		while (insn->first_spill)
		{
			Regmove* s = insn->first_spill;
			insn->first_spill = s->next;
			free(s);
		}
	}
}

void discard(struct midnode* node)
{
	if (!node)
		return;
	if (node->left)
		discard(node->left);
	if (node->right)
		discard(node->right);
	free(node);
}

void generate_finalise(void)
{
	arch_emit_comment("max nodes = %d, max instructions = %d", maxnodecount, maxinstructioncount);
}
