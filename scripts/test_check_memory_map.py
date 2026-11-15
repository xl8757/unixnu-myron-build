#!/usr/bin/env python3
"""Tests for scripts/check-memory-map.py.

The memory map is the one table where a wrong answer corrupts a boot rather
than failing a build, so the comparison it drives is tested rather than
assumed. The cases below are the four ways the map can be wrong: a reservation
nobody covers, two entries fighting over the same range, an entry covering
nothing, and an entry reaching outside DRAM.

Run: python3 scripts/test_check_memory_map.py -v
"""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

spec = importlib.util.spec_from_file_location('check_memory_map',
                                              HERE / 'check-memory-map.py')
memory_map = importlib.util.module_from_spec(spec)
spec.loader.exec_module(memory_map)

Region = memory_map.Region
UEFI_REGIONS = memory_map.UEFI_REGIONS

DRAM = Region('DRAM', 0x80000000, 0x80000000, 'test')


def dram_region(name: str, address: int, length: int) -> Region:
    return Region(name, address, length, 'map', 'SYS_MEM')


class AuditTest(unittest.TestCase):
    def test_map_covering_every_reservation_has_no_problem(self):
        # Arrange
        descriptors = [dram_region('HYP', 0x80000000, 0x00E00000),
                       dram_region('DXE Heap', 0xB8000000, 0x1FCC0000)]
        reservations = [Region('gunyah_hyp_region', 0x80000000, 0x00E00000, 'dtb'),
                        Region('ramoops', 0xD7E00000, 0x00200000, 'dtb')]
        descriptors.append(dram_region('Ramoops', 0xD7E00000, 0x00200000))

        # Act
        problems, claimed, unexplained = memory_map.audit(descriptors, reservations, DRAM)

        # Assert
        self.assertEqual(problems, [])
        self.assertEqual(len(claimed), 2)
        self.assertEqual(unexplained, [])

    def test_uncovered_reservation_is_reported(self):
        # Arrange
        descriptors = [dram_region('DXE Heap', 0xB8000000, 0x1FCC0000)]
        reservations = [Region('ramoops', 0xD7E00000, 0x00200000, 'dtb')]

        # Act
        problems, _, _ = memory_map.audit(descriptors, reservations, DRAM)

        # Assert
        self.assertTrue(any('not covered' in problem for problem in problems), problems)

    def test_partially_covered_reservation_is_reported(self):
        # Arrange: a descriptor that starts inside the reservation and ends
        # inside it leaves both halves of the reservation uncovered.
        descriptors = [dram_region('Ramoops', 0xD7E01000, 0x00001000)]
        reservations = [Region('ramoops', 0xD7E00000, 0x00200000, 'dtb')]

        # Act
        problems, _, _ = memory_map.audit(descriptors, reservations, DRAM)

        # Assert
        self.assertTrue(any('not covered' in problem for problem in problems), problems)

    def test_overlapping_descriptors_are_reported(self):
        # Arrange
        descriptors = [dram_region('DXE Heap', 0xB8000000, 0x1FCC0000),
                       dram_region('Kernel', 0xD7CB0000, 0x00100000)]

        # Act
        problems, _, _ = memory_map.audit(descriptors, [], DRAM)

        # Assert
        self.assertTrue(any('descriptors overlap' in problem for problem in problems), problems)

    def test_one_descriptor_may_group_several_reservations(self):
        # Arrange: nx729jPkg groups the device tree's fine-grained nodes.
        descriptors = [dram_region('XBL AOP', 0x81A00000, 0x00260000)]
        reservations = [Region('xbl_dtlog', 0x81A00000, 0x00040000, 'dtb'),
                        Region('xbl_ramdump', 0x81A40000, 0x001C0000, 'dtb'),
                        Region('aop_image', 0x81C00000, 0x00060000, 'dtb')]

        # Act
        problems, claimed, _ = memory_map.audit(descriptors, reservations, DRAM)

        # Assert
        self.assertEqual(problems, [])
        self.assertEqual(len(claimed), 1)

    def test_descriptor_outside_dram_is_reported(self):
        # Arrange
        descriptors = [dram_region('DDR Hole', 0x100000000, 0x00100000)]

        # Act
        problems, _, _ = memory_map.audit(descriptors, [], DRAM)

        # Assert
        self.assertTrue(any('not inside the DRAM window' in problem for problem in problems),
                        problems)

    def test_register_region_outside_dram_is_not_reported(self):
        # Arrange
        descriptors = [Region('TLMM', 0x0F000000, 0x01000000, 'map', 'MMAP_IO')]

        # Act
        problems, _, _ = memory_map.audit(descriptors, [], DRAM)

        # Assert
        self.assertEqual(problems, [])

    def test_uefi_region_covering_nothing_is_accepted(self):
        # Arrange
        descriptors = [dram_region(name, 0xA7000000 + index * 0x1000, 0x1000)
                       for index, name in enumerate(UEFI_REGIONS)]

        # Act
        problems, _, unexplained = memory_map.audit(descriptors, [], DRAM)

        # Assert
        self.assertEqual(problems, [])
        self.assertEqual(unexplained, [])

    def test_unknown_region_covering_nothing_is_reported(self):
        # Arrange
        descriptors = [dram_region('Mystery', 0xA7000000, 0x1000)]

        # Act
        problems, _, unexplained = memory_map.audit(descriptors, [], DRAM)

        # Assert
        self.assertEqual([region.name for region in unexplained], ['Mystery'])
        self.assertTrue(any('covers no reservation' in problem for problem in problems), problems)


class MapSourceTest(unittest.TestCase):
    def test_descriptors_are_read_out_of_the_map_source(self):
        # Arrange
        source = HERE.parent / 'firmware/Platforms/ZTE/nx769jPkg/Library/MemoryMapLib/MemoryMapLib.c'

        # Act
        regions = memory_map.map_regions(source)

        # Assert
        self.assertGreater(len(regions), 20)
        by_name = {region.name: region for region in regions}
        self.assertIn('DXE Heap', by_name)
        self.assertEqual(by_name['DXE Heap'].address, 0xB8000000)
        # The register regions are MMIO and so stay out of the DRAM audit.
        self.assertFalse(by_name['TLMM'].dram)
        self.assertTrue(by_name['HYP'].dram)

    def test_boot_image_reservations_are_read_out_of_board_toml(self):
        # Arrange
        board = HERE.parent / 'board.toml'

        # Act
        regions = memory_map.boot_image_reservations(board)

        # Assert
        by_name = {region.name: region for region in regions}
        self.assertIn('splash_region', by_name)
        self.assertEqual(by_name['splash_region'].address, 0xE3940000)
        self.assertEqual(by_name['splash_region'].length, 0x2B00000)


if __name__ == '__main__':
    unittest.main()
