#
# Author: Harti Brandt <harti@freebsd.org>

.include <src.opts.mk>

CONTRIB= ${SRCTOP}/contrib/bsnmp/lib
.PATH: ${CONTRIB}

LIB=	bsnmp
SHLIB_MAJOR=	7
LD_FATAL_WARNINGS=	no

CFLAGS+= -I${CONTRIB} -DHAVE_ERR_H -DHAVE_GETADDRINFO -DHAVE_STRLCPY
CFLAGS+= -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DQUADFMT='"llu"' -DQUADXFMT='"llx"'

CFLAGS+= -DBOGUS_CVE_2019_5610_FIX

.if ${MK_OPENSSL} != "no"
CFLAGS+= -DHAVE_LIBCRYPTO
LIBADD=	crypto
.endif

SRCS=	asn1.c snmp.c snmpagent.c snmpclient.c snmpcrypto.c support.c
SRCS+=	snmptc.h
INCS=	asn1.h snmp.h snmpagent.h snmpclient.h
MAN=	asn1.3 bsnmpagent.3 bsnmpclient.3 bsnmplib.3

.if ${MK_DIRDEPS_BUILD} == "yes"
GENSNMPTREE?= ${HOST_OBJTOP}/usr.sbin/bsnmpd/gensnmptree/gensnmptree
.endif
GENSNMPTREE?= gensnmptree

snmptc.h : tc.def
	(\
	echo "/* autogenerated from tc.def */";\
	echo "#ifndef snmptc_h_1529923773";\
	echo "#define snmptc_h_1529923773";\
	${GENSNMPTREE} -E -f <${.ALLSRC};\
	echo "#endif" ; \
	) >${.TARGET}

CLEANFILES+= snmptc.h

MLINKS+=	asn1.3	asn_append_oid.3
MLINKS+=	asn1.3	asn_commit_header.3
MLINKS+=	asn1.3	asn_compare_oid.3
MLINKS+=	asn1.3	asn_get_counter64_raw.3
MLINKS+=	asn1.3	asn_get_header.3
MLINKS+=	asn1.3	asn_get_integer.3
MLINKS+=	asn1.3	asn_get_integer_raw.3
MLINKS+=	asn1.3	asn_get_ipaddress.3
MLINKS+=	asn1.3	asn_get_ipaddress_raw.3
MLINKS+=	asn1.3	asn_get_null.3
MLINKS+=	asn1.3	asn_get_null_raw.3
MLINKS+=	asn1.3	asn_get_objid.3
MLINKS+=	asn1.3	asn_get_objid_raw.3
MLINKS+=	asn1.3	asn_get_octetstring.3
MLINKS+=	asn1.3	asn_get_octetstring_raw.3
MLINKS+=	asn1.3	asn_get_sequence.3
MLINKS+=	asn1.3	asn_get_timeticks.3
MLINKS+=	asn1.3	asn_get_uint32_raw.3
MLINKS+=	asn1.3	asn_is_suboid.3
MLINKS+=	asn1.3	asn_oid2str.3
MLINKS+=	asn1.3	asn_oid2str_r.3
MLINKS+=	asn1.3	asn_put_counter64.3
MLINKS+=	asn1.3	asn_put_exception.3
MLINKS+=	asn1.3	asn_put_header.3
MLINKS+=	asn1.3	asn_put_integer.3
MLINKS+=	asn1.3	asn_put_ipaddress.3
MLINKS+=	asn1.3	asn_put_null.3
MLINKS+=	asn1.3	asn_put_objid.3
MLINKS+=	asn1.3	asn_put_octetstring.3
MLINKS+=	asn1.3	asn_put_temp_header.3
MLINKS+=	asn1.3	asn_put_timeticks.3
MLINKS+=	asn1.3	asn_put_uint32.3
MLINKS+=	asn1.3	asn_skip.3
MLINKS+=	asn1.3	asn_slice_oid.3

MLINKS+=	bsnmpagent.3	snmp_debug.3
MLINKS+=	bsnmpagent.3	snmp_dep_commit.3
MLINKS+=	bsnmpagent.3	snmp_dep_finish.3
MLINKS+=	bsnmpagent.3	snmp_dep_lookup.3
MLINKS+=	bsnmpagent.3	snmp_dep_rollback.3
MLINKS+=	bsnmpagent.3	snmp_depop_t.3
MLINKS+=	bsnmpagent.3	snmp_get.3
MLINKS+=	bsnmpagent.3	snmp_getbulk.3
MLINKS+=	bsnmpagent.3	snmp_getnext.3
MLINKS+=	bsnmpagent.3	snmp_init_context.3
MLINKS+=	bsnmpagent.3	snmp_make_errresp.3
MLINKS+=	bsnmpagent.3	snmp_op_t.3
MLINKS+=	bsnmpagent.3	snmp_set.3
MLINKS+=	bsnmpagent.3	snmp_trace.3
MLINKS+=	bsnmpagent.3	tree_size.3

MLINKS+=	bsnmpclient.3	snmp_add_binding.3
MLINKS+=	bsnmpclient.3	snmp_client.3
MLINKS+=	bsnmpclient.3	snmp_client_init.3
MLINKS+=	bsnmpclient.3	snmp_client_set_host.3
MLINKS+=	bsnmpclient.3	snmp_client_set_port.3
MLINKS+=	bsnmpclient.3	snmp_close.3
MLINKS+=	bsnmpclient.3	snmp_dialog.3
MLINKS+=	bsnmpclient.3	snmp_discover_engine.3
MLINKS+=	bsnmpclient.3	snmp_oid_append.3
MLINKS+=	bsnmpclient.3	snmp_open.3
MLINKS+=	bsnmpclient.3	snmp_parse_server.3
MLINKS+=	bsnmpclient.3	snmp_pdu_check.3
MLINKS+=	bsnmpclient.3	snmp_pdu_create.3
MLINKS+=	bsnmpclient.3	snmp_pdu_send.3
MLINKS+=	bsnmpclient.3	snmp_receive.3
MLINKS+=	bsnmpclient.3	snmp_send_cb_f.3
MLINKS+=	bsnmpclient.3	snmp_table_cb_f.3
MLINKS+=	bsnmpclient.3	snmp_table_fetch.3
MLINKS+=	bsnmpclient.3	snmp_table_fetch_async.3
MLINKS+=	bsnmpclient.3	snmp_timeout_cb_f.3
MLINKS+=	bsnmpclient.3	snmp_timeout_start_f.3
MLINKS+=	bsnmpclient.3	snmp_timeout_stop_f.3

MLINKS+=	bsnmplib.3	TRUTH_GET.3
MLINKS+=	bsnmplib.3	TRUTH_MK.3
MLINKS+=	bsnmplib.3	TRUTH_OK.3
MLINKS+=	bsnmplib.3	snmp_calc_keychange.3
MLINKS+=	bsnmplib.3	snmp_get_local_keys.3
MLINKS+=	bsnmplib.3	snmp_passwd_to_keys.3
MLINKS+=	bsnmplib.3	snmp_pdu_decode.3
MLINKS+=	bsnmplib.3	snmp_pdu_decode_header.3
MLINKS+=	bsnmplib.3	snmp_pdu_decode_scoped.3
MLINKS+=	bsnmplib.3	snmp_pdu_decode_secmode.3
MLINKS+=	bsnmplib.3	snmp_pdu_dump.3
MLINKS+=	bsnmplib.3	snmp_pdu_encode.3
MLINKS+=	bsnmplib.3	snmp_pdu_free.3
MLINKS+=	bsnmplib.3	snmp_pdu_init_secparams.3
MLINKS+=	bsnmplib.3	snmp_value_copy.3
MLINKS+=	bsnmplib.3	snmp_value_free.3
MLINKS+=	bsnmplib.3	snmp_value_parse.3

FILESGROUPS+=	DEFS
DEFS=	tc.def
DEFSDIR?=	${SHAREDIR}/snmp/defs
DEFSPACKAGE=	bsnmp

.include <bsd.lib.mk>
