# SPDX-License-Identifier: GPL-2.0+
# Copyright (c) 2016 Google, Inc
# Written by Simon Glass <sjg@chromium.org>
#
# Entry-type module for producing a FIT
#

from collections import defaultdict, OrderedDict
import libfdt

from binman.entry import Entry
from dtoc import fdt_util
from dtoc.fdt import Fdt
from patman import tools

class Entry_fit(Entry):
    """Entry containing a FIT

    This calls mkimage to create a FIT (U-Boot Flat Image Tree) based on the
    input provided.

    Nodes for the FIT should be written out in the binman configuration just as
    they would be in a file passed to mkimage.

    For example, this creates an image containing a FIT with U-Boot SPL:

        binman {
            fit {
                description = "Test FIT";

                images {
                    kernel@1 {
                        description = "SPL";
                        os = "u-boot";
                        type = "rkspi";
                        arch = "arm";
                        compression = "none";
                        load = <0>;
                        entry = <0>;

                        u-boot-spl {
                        };
                    };
                };
            };
        };

    Properties:
        fit,external-offset: Indicates that the contents of the FIT are external
            and provides the external offset. This is passsed to mkimage via
            the -E and -p flags.

    """
    def __init__(self, section, etype, node):
        """
        Members:
            _fit: FIT file being built
            _fit_sections: dict:
                key: relative path to entry Node (from the base of the FIT)
                value: Entry_section object comprising the contents of this
                    node
        """
        super().__init__(section, etype, node)
        self._fit = None
        self._fit_sections = {}
        self._fit_props = {}

    def ReadNode(self):
        self._ReadSubnodes()
        super().ReadNode()

    def _ReadSubnodes(self):
        def _AddNode(base_node, depth, node):
            """Add a node to the FIT

            Args:
                base_node: Base Node of the FIT (with 'description' property)
                depth: Current node depth (0 is the base node)
                node: Current node to process

            There are two cases to deal with:
                - hash and signature nodes which become part of the FIT
                - binman entries which are used to define the 'data' for each
                  image
            """
            for pname, prop in node.props.items():
                if pname.startswith('fit,'):
                    self._fit_props[pname] = prop
                else:
                    fsw.property(pname, prop.bytes)

            rel_path = node.path[len(base_node.path):]
            has_images = depth == 2 and rel_path.startswith('/images/')
            if has_images:
                # This node is a FIT subimage node (e.g. "/images/kernel")
                # containing content nodes. We collect the subimage nodes and
                # section entries for them here to merge the content subnodes
                # together and put the merged contents in the subimage node's
                # 'data' property later.
                entry = Entry.Create(self.section, node, etype='section')
                entry.ReadNode()
                self._fit_sections[rel_path] = entry

            for subnode in node.subnodes:
                if has_images and not (subnode.name.startswith('hash') or
                                       subnode.name.startswith('signature')):
                    # This subnode is a content node not meant to appear in
                    # the FIT (e.g. "/images/kernel/u-boot"), so don't call
                    # fsw.add_node() or _AddNode() for it.
                    pass
                else:
                    with fsw.add_node(subnode.name):
                        _AddNode(base_node, depth + 1, subnode)

        # Build a new tree with all nodes and properties starting from the
        # entry node
        fsw = libfdt.FdtSw()
        fsw.finish_reservemap()
        with fsw.add_node(''):
            _AddNode(self._node, 0, self._node)
        fdt = fsw.as_fdt()

        # Pack this new FDT and scan it so we can add the data later
        fdt.pack()
        self._fdt = Fdt.FromData(fdt.as_bytearray())
        self._fdt.Scan()

    def ObtainContents(self):
        """Obtain the contents of the FIT

        This adds the 'data' properties to the input ITB (Image-tree Binary)
        then runs mkimage to process it.
        """
        # self._BuildInput() either returns bytes or raises an exception.
        data = self._BuildInput(self._fdt)
        uniq = self.GetUniqueName()
        input_fname = tools.GetOutputFilename('%s.itb' % uniq)
        output_fname = tools.GetOutputFilename('%s.fit' % uniq)
        tools.WriteFile(input_fname, data)
        tools.WriteFile(output_fname, data)

        args = []
        ext_offset = self._fit_props.get('fit,external-offset')
        if ext_offset is not None:
            args += ['-E', '-p', '%x' % fdt_util.fdt32_to_cpu(ext_offset.value)]
        tools.Run('mkimage', '-t', '-F', output_fname, *args)

        self.SetContents(tools.ReadFile(output_fname))
        return True

    def _BuildInput(self, fdt):
        """Finish the FIT by adding the 'data' properties to it

        Arguments:
            fdt: FIT to update

        Returns:
            New fdt contents (bytes)
        """
        for path, section in self._fit_sections.items():
            node = fdt.GetNode(path)
            # Entry_section.ObtainContents() either returns True or
            # raises an exception.
            section.ObtainContents()
            section.Pack(0)
            data = section.GetData()
            node.AddData('data', data)

        fdt.Sync(auto_resize=True)
        data = fdt.GetContents()
        return data

    def CheckMissing(self, missing_list):
        """Check if any entries in this FIT have missing external blobs

        If there are missing blobs, the entries are added to the list

        Args:
            missing_list: List of Entry objects to be added to
        """
        for path, section in self._fit_sections.items():
            section.CheckMissing(missing_list)

    def SetAllowMissing(self, allow_missing):
        for section in self._fit_sections.values():
            section.SetAllowMissing(allow_missing)
